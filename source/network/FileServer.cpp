#include "FileServer.hpp"
#include "HttpUtil.hpp"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/time.h>
#include <cerrno>
#include <whb/log.h>
#include <coreinit/debug.h>

#define QRLOG(fmt, ...) do { \
    OSReport("[QR] " fmt "\n", ##__VA_ARGS__); \
    WHBLogPrintf("[QR] " fmt, ##__VA_ARGS__); \
} while(0)

FileServer::FileServer() {}
FileServer::~FileServer() { Stop(); }

std::string FileServer::GetIPAddress() const {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return "0.0.0.0";
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &addr.sin_addr);
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock);
        return "0.0.0.0";
    }
    struct sockaddr_in local;
    socklen_t len = sizeof(local);
    char ip[16] = {0};
    if (getsockname(sock, (struct sockaddr*)&local, &len) == 0)
        inet_ntop(AF_INET, &local.sin_addr, ip, sizeof(ip));
    close(sock);
    return ip;
}

bool FileServer::Start(int port) {
    if (mRunning) Stop();
    mServerFd = socket(AF_INET, SOCK_STREAM, 0);
    if (mServerFd < 0) { QRLOG("socket failed: %d", errno); return false; }
    int opt = 1;
    setsockopt(mServerFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(mServerFd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        QRLOG("bind port %d failed: %d", port, errno);
        close(mServerFd); mServerFd = -1; return false;
    }
    if (listen(mServerFd, 1) < 0) {
        QRLOG("listen failed: %d", errno);
        close(mServerFd); mServerFd = -1; return false;
    }
    mPort = port;
    mRunning = true;
    mStatus = Status::Idle;
    return true;
}

void FileServer::Stop() {
    mRunning = false;
    mStatus = Status::Idle;
    if (mServerFd >= 0) { close(mServerFd); mServerFd = -1; }
}

FileServer::Status FileServer::GetStatus() const {
    return mStatus.load();
}

int FileServer::GetProgress() const {
    return mProgress.load();
}

std::string FileServer::GetClientIP() const {
    std::lock_guard<std::mutex> lock(mMutex);
    return mClientIP;
}

void FileServer::ConfirmTransfer() {
    mConfirmTransfer = true;
}

void FileServer::PrepareForNextFile() {
    mStatus = Status::Waiting;
    mProgress = 0;
}

// Single-file client handler

int FileServer::HandleClient(int clientFd, const std::string& filePath) {
    char request[2048];
    int received = recv(clientFd, request, sizeof(request) - 1, 0);
    if (received <= 0) {
        QRLOG("HandleClient: recv failed (rc=%d errno=%d)", received, errno);
        close(clientFd); return -1;
    }
    request[received] = '\0';

    char method[16] = {0}, path[512] = {0};
    sscanf(request, "%15s %511s", method, path);
    QRLOG("HandleClient: req=%.50s", request);

    FILE* f = fopen(filePath.c_str(), "rb");
    if (!f) {
        QRLOG("HandleClient: fopen failed '%s' (errno=%d)", filePath.c_str(), errno);
        const char* resp = "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\n"
                           "Content-Length: 9\r\nConnection: close\r\n\r\nNot Found";
        send(clientFd, resp, strlen(resp), 0);
        close(clientFd);
        return -1;
    }
    QRLOG("HandleClient: fopen OK");

    fseek(f, 0, SEEK_END);
    long fileSize = ftell(f);
    fseek(f, 0, SEEK_SET);

    std::string fn = filePath;
    size_t s = fn.rfind('/');
    if (s != std::string::npos) fn = fn.substr(s + 1);

    const char* ct = GetContentType(fn);

    QRLOG("HandleClient: fileSize=%ld ct=%s fn=%s", fileSize, ct, fn.c_str());

    bool isHead = (method[0] == 'H' && method[1] == 'E' && method[2] == 'A' && method[3] == 'D' && method[4] == '\0');

    if (SendHttpHeaders(clientFd, ct, fn.c_str(), fileSize) < 0) {
        QRLOG("HandleClient: header send failed (errno=%d)", errno);
        fclose(f); close(clientFd); return -1;
    }

    if (isHead) {
        QRLOG("HandleClient: HEAD request, headers sent only");
        fclose(f); close(clientFd); return 2;
    }

    char buf[32768];
    long sent = 0;
    int n;
    while (mRunning && (n = (int)fread(buf, 1, sizeof(buf), f)) > 0) {
        int off = 0;
        while (off < n) {
            int w = send(clientFd, buf + off, n - off, 0);
            if (w < 0) {
                QRLOG("HandleClient: send failed at sent=%ld (errno=%d)", sent, errno);
                fclose(f); close(clientFd); return -1;
            }
            off += w;
        }
        sent += n;
        mProgress = (int)(sent * 100 / fileSize);
    }
    QRLOG("HandleClient: transfer complete (%ld bytes)", sent);
    fclose(f);
    close(clientFd);
    return 1;
}

// Listing page

std::string FileServer::BuildListingPage(const std::vector<std::string>& names,
                                         int fileCount) const {
    std::string html;
    html += "<html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<style>body{font-family:sans-serif;background:#f5f5f5;margin:0;padding:20px;text-align:center}"
            "h2{color:#333}ul{list-style:none;padding:0}"
            "li{margin:10px 0}"
            ".fl{display:block;padding:14px;background:#fff;border-radius:8px;cursor:pointer;"
            "color:#009ac7;text-decoration:none;font-size:18px;box-shadow:0 1px 3px rgba(0,0,0,0.12)}"
            ".fl:hover{background:#e0f4fb}</style></head><body>"
            "<h2>Wii U Album Transfer</h2><p style='color:#666'>"
            + std::to_string(fileCount) + " file(s) ready</p><ul>";
    for (size_t i = 0; i < names.size(); i++) {
        html += "<li><span class='fl' data-url='/" + std::to_string(i) + "'>"
                + names[i] + "</span></li>";
    }
    html += "</ul>"
            "<script>document.querySelectorAll('.fl').forEach(function(el){"
            "el.addEventListener('click',function(){"
            "window.location.href=this.dataset.url+'?t='+Date.now();"
            "});});</script>"
            "</body></html>";
    return html;
}

void FileServer::ServeListingPage(int clientFd,
                                  const std::vector<std::string>& names,
                                  int fileCount) {
    std::string html = BuildListingPage(names, fileCount);
    SendHttpResponse(clientFd, 200, "text/html; charset=utf-8",
                     html.c_str(), (int)html.size());
    close(clientFd);
}

// Multi-file file content

void FileServer::ServeFileContent(int clientFd, int fileIdx,
                                  const std::vector<std::string>& filePaths,
                                  const std::vector<std::string>& names,
                                  const char* clientIP) {
    QRLOG("ServeMulti: serving file %d '%s' to %s", fileIdx, filePaths[fileIdx].c_str(), clientIP);
    mCurrentFileIdx = fileIdx;

    FILE* f = fopen(filePaths[fileIdx].c_str(), "rb");
    if (!f) {
        QRLOG("ServeMulti: fopen failed '%s' (errno=%d)", filePaths[fileIdx].c_str(), errno);
        const char* resp404 = "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\n"
                              "Content-Length: 9\r\nConnection: close\r\n\r\nNot Found";
        send(clientFd, resp404, strlen(resp404), 0);
        close(clientFd);
        mStatus = Status::Error;
        return;
    }

    fseek(f, 0, SEEK_END);
    long fileSize = ftell(f);
    fseek(f, 0, SEEK_SET);

    const char* ct = GetContentType(names[fileIdx]);

    if (SendHttpHeaders(clientFd, ct, names[fileIdx].c_str(), fileSize) < 0) {
        QRLOG("ServeMulti: header send failed (errno=%d)", errno);
        fclose(f); close(clientFd);
        mStatus = Status::Error;
        return;
    }

    char buf[32768];
    long sent = 0;
    int n;
    while ((n = (int)fread(buf, 1, sizeof(buf), f)) > 0) {
        int off = 0;
        while (off < n) {
            int w = send(clientFd, buf + off, n - off, 0);
            if (w < 0) break;
            off += w;
        }
        if (off < n) break;
        if (sent == 0) {
            mStatus = Status::Transferring;
            mProgress = 0;
        }
        sent += n;
        mProgress = (int)(sent * 100 / fileSize);
    }
    fclose(f);
    close(clientFd);
    QRLOG("ServeMulti: file %d sent (%ld bytes)", fileIdx, sent);

    if (sent == 0) {
        QRLOG("ServeMulti: 0 bytes sent -- likely browser prefetch, ignoring");
        return;
    }

    if (sent >= fileSize) {
        mProgress = 100;
        mServedCount++;
        mStatus = Status::Done;
        QRLOG("ServeMulti: file %d done (served %d/%d)", fileIdx, mServedCount.load(), mTotalFiles.load());
    } else {
        QRLOG("ServeMulti: file %d partial (%ld/%ld) -- browser may retry", fileIdx, sent, fileSize);
    }
}

// Single-file serve loop

void FileServer::ServeFile(const std::string& filePath) {
    if (!mRunning || mServerFd < 0) { mStatus = Status::Error; return; }
    mCurrentFile = filePath;
    mStatus = Status::Waiting;
    mProgress = 0;
    mConfirmTransfer = false;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mClientIP.clear();
    }

    QRLOG("ServeFile: waiting for client on port %d", mPort);

    while (mRunning) {
        struct timeval tv = {5, 0};
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(mServerFd, &fds);

        int r = select(mServerFd + 1, &fds, nullptr, nullptr, &tv);
        if (!mRunning) break;
        if (r <= 0) continue;

        struct sockaddr_in ca;
        socklen_t cl = sizeof(ca);
        int cf = accept(mServerFd, (struct sockaddr*)&ca, &cl);
        if (cf < 0) continue;

        char cip[32] = {0};
        inet_ntop(AF_INET, &ca.sin_addr, cip, sizeof(cip));
        {
            std::lock_guard<std::mutex> lock(mMutex);
            if (mClientIP.empty()) mClientIP = cip;
        }

        if (mConfirmTransfer) {
            QRLOG("ServeFile: confirmed, sending '%s'", filePath.c_str());
            mStatus = Status::Transferring;
            int rc = HandleClient(cf, filePath);
            QRLOG("ServeFile: HandleClient returned %d", rc);
            close(cf);
            if (rc == 1) {
                mStatus = Status::Done;
                mProgress = 100;
                break;
            } else if (rc == 2) {
                mStatus = Status::Transferring;
                continue;
            } else {
                mStatus = Status::Error;
                break;
            }
        }

        char request[2048];
        int received = recv(cf, request, sizeof(request) - 1, 0);
        if (received <= 0) { close(cf); continue; }
        request[received] = '\0';

        const char *html =
            "<html><head>"
            "<meta http-equiv='refresh' content='2'>"
            "<meta name='viewport' content='width=device-width,initial-scale=1'>"
            "</head><body style='text-align:center;padding-top:80px;"
            "font-family:sans-serif;background:#f5f5f5;margin:0'>"
            "<h2 style='color:#333;font-size:24px'>Wii U Album Transfer</h2>"
            "<p style='color:#666;font-size:18px'>Connected to Wii U</p>"
            "<p style='color:#999;font-size:14px'>"
            "Press A on your Wii U console to begin</p>"
            "</body></html>";
        SendHttpResponse(cf, 200, "text/html; charset=utf-8",
                         html, (int)strlen(html));
        close(cf);
    }

    if (!mRunning) mStatus = Status::Idle;
}

// Multi-file serve loop

void FileServer::ServeMulti(const std::vector<std::string>& filePaths) {
    if (!mRunning || mServerFd < 0) { mStatus = Status::Error; return; }
    if (filePaths.empty()) { mStatus = Status::Error; return; }
    mStatus = Status::Waiting;
    mProgress = 0;
    mConfirmTransfer = false;
    mTotalFiles = (int)filePaths.size();
    mCurrentFileIdx = 0;
    mServedCount = 0;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mClientIP.clear();
    }

    std::vector<std::string> names;
    for (auto& fp : filePaths) {
        std::string fn = fp;
        size_t sep = fn.rfind('/');
        if (sep != std::string::npos) fn = fn.substr(sep + 1);
        names.push_back(fn);
    }

    QRLOG("ServeMulti: waiting for client on port %d (%d files)", mPort, mTotalFiles.load());

    while (mRunning) {
        struct timeval tv = {5, 0};
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(mServerFd, &fds);

        int r = select(mServerFd + 1, &fds, nullptr, nullptr, &tv);
        if (!mRunning) break;
        if (r <= 0) continue;

        struct sockaddr_in ca;
        socklen_t cl = sizeof(ca);
        int cf = accept(mServerFd, (struct sockaddr*)&ca, &cl);
        if (cf < 0) continue;

        char cip[32] = {0};
        inet_ntop(AF_INET, &ca.sin_addr, cip, sizeof(cip));
        {
            std::lock_guard<std::mutex> lock(mMutex);
            if (mClientIP.empty()) mClientIP = cip;
        }

        char request[4096];
        int received = recv(cf, request, sizeof(request) - 1, 0);
        if (received <= 0) { close(cf); continue; }
        request[received] = '\0';

        char method[16] = {0}, rawPath[512] = {0};
        sscanf(request, "%15s %511s", method, rawPath);

        std::string path = rawPath;
        size_t qpos = path.find('?');
        if (qpos != std::string::npos) path = path.substr(0, qpos);

        if (path == "/" || path.empty()) {
            ServeListingPage(cf, names, (int)filePaths.size());
            QRLOG("ServeMulti: served listing page to %s", cip);
        } else {
            int fileIdx = ParseFileIndex(path);
            if (fileIdx >= 0 && fileIdx < (int)filePaths.size()) {
                ServeFileContent(cf, fileIdx, filePaths, names, cip);
            } else {
                ServeListingPage(cf, names, (int)filePaths.size());
                QRLOG("ServeMulti: served listing page to %s (unknown path)", cip);
            }
        }
    }

    if (!mRunning) mStatus = Status::Idle;
}
