#pragma once
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>

class FileServer {
public:
    enum class Status { Idle, Waiting, Transferring, Done, Error };

    FileServer();
    ~FileServer();

    bool        Start(int port);
    void        Stop();
    bool        IsRunning() const { return mRunning; }
    int         GetPort()   const { return mPort; }
    std::string GetIPAddress() const;

    void ServeFile(const std::string& filePath);
    void ServeMulti(const std::vector<std::string>& filePaths);
    void ConfirmTransfer();
    void PrepareForNextFile();
    bool IsTransferConfirmed() const { return mConfirmTransfer.load(); }

    Status      GetStatus()   const;
    int         GetProgress() const;
    std::string GetClientIP() const;
    int         GetCurrentFileIdx() const { return mCurrentFileIdx.load(); }
    int         GetTotalFiles() const { return mTotalFiles.load(); }
    int         GetServedCount() const { return mServedCount.load(); }

private:
    int  mServerFd = -1;
    int  mPort     = 0;
    std::atomic<bool>  mRunning{false};
    std::atomic<Status> mStatus{Status::Idle};
    std::atomic<int>    mProgress{0};
    std::atomic<bool>   mConfirmTransfer{false};
    std::atomic<int>    mCurrentFileIdx{0};
    std::atomic<int>    mTotalFiles{0};
    std::atomic<int>    mServedCount{0};
    std::string mClientIP;
    std::string mCurrentFile;
    mutable std::mutex mMutex;

    int  HandleClient(int clientFd, const std::string& filePath);
    std::string BuildListingPage(const std::vector<std::string>& names,
                                 int fileCount) const;
    void ServeListingPage(int clientFd,
                          const std::vector<std::string>& names,
                          int fileCount);
    void ServeFileContent(int clientFd, int fileIdx,
                          const std::vector<std::string>& filePaths,
                          const std::vector<std::string>& names,
                          const char* clientIP);
};
