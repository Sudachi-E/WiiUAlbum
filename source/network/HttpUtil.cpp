#include "HttpUtil.hpp"
#include <cstdio>
#include <cstring>
#include <cctype>
#include <unistd.h>
#include <sys/socket.h>

const char* GetContentType(const std::string& filename) {
    size_t dot = filename.rfind('.');
    if (dot == std::string::npos) return "application/octet-stream";

    std::string ext = filename.substr(dot);
    for (auto& c : ext) c = (char)tolower(c);

    if (ext == ".png")  return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".mp4")  return "video/mp4";
    if (ext == ".avi")  return "video/x-msvideo";
    if (ext == ".mkv")  return "video/x-matroska";
    if (ext == ".mov")  return "video/quicktime";
    return "application/octet-stream";
}

int SendHttpResponse(int fd, int status, const char* contentType,
                     const char* body, int bodyLen) {
    const char* statusText;
    switch (status) {
        case 200: statusText = "OK"; break;
        case 404: statusText = "Not Found"; break;
        default:  statusText = "OK"; break;
    }

    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "Cache-Control: no-cache, no-store\r\n\r\n",
        status, statusText, contentType, bodyLen);

    if (send(fd, header, hlen, 0) < 0) return -1;
    if (bodyLen > 0 && body) {
        int sent = 0;
        while (sent < bodyLen) {
            int w = send(fd, body + sent, bodyLen - sent, 0);
            if (w < 0) return -1;
            sent += w;
        }
    }
    return 0;
}

int SendHttpHeaders(int fd, const char* contentType,
                    const char* contentDisposition, long contentLength) {
    char header[1024];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\nContent-Type: %s\r\n"
        "Content-Disposition: attachment; filename=\"%s\"\r\n"
        "Content-Length: %ld\r\nConnection: close\r\n"
        "Accept-Ranges: bytes\r\nCache-Control: no-cache\r\n\r\n",
        contentType, contentDisposition, contentLength);
    return send(fd, header, hlen, 0) < 0 ? -1 : 0;
}

int ParseFileIndex(const std::string& path) {
    if (path.size() > 1 && path[0] == '/') {
        const char* p = path.c_str() + 1;
        bool allDigits = true;
        for (const char* c = p; *c; c++) {
            if (*c < '0' || *c > '9') { allDigits = false; break; }
        }
        if (allDigits && *p) return atoi(p);
    }
    return -1;
}
