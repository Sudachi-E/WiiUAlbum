#include "Album.hpp"
#include "../ui/Glyphs.hpp"
#include <cstdio>
#include <whb/log.h>
#include <coreinit/debug.h>

void Album::StartQRTransfer(int filteredIdx) {
    if (filteredIdx < 0 || filteredIdx >= (int)mFiltered.size()) return;

    mQRItemIdx = filteredIdx;
    int idx = mFiltered[filteredIdx];
    auto& item = mAllItems[idx];
    mQRFileName = item.filename;

    std::string ip = mFileServer.GetIPAddress();
    if (ip == "0.0.0.0") {
        mQRState = QRState::Error;
        return;
    }

    int port = 8080;
    mQRUrl = "http://" + ip + ":" + std::to_string(port) + "/";

    if (!mQRCode.Encode(mQRUrl)) {
        mQRState = QRState::Error;
        return;
    }

    if (mQRCodeTexture) { SDL_DestroyTexture(mQRCodeTexture); mQRCodeTexture = nullptr; }
    mQRCodeTexture = mQRCode.Render(Gfx::GetRenderer(), 12);
    if (!mQRCodeTexture) {
        mQRState = QRState::Error;
        return;
    }

    if (!mFileServer.Start(port)) {
        mQRState = QRState::Error;
        return;
    }

    mQRState = QRState::ShowQR;

    std::string filePath = item.path;
    std::thread srvThread([this, filePath]() {
        mFileServer.ServeFile(filePath);
    });
    srvThread.detach();
}

void Album::StartQRMultiTransfer(const std::vector<int>& filteredIndices) {
    if (filteredIndices.empty()) return;
    mTransferMode = TransferMode::Multi;

    mTransferFilePaths.clear();
    std::string firstFileName;
    for (int fi : filteredIndices) {
        if (fi < 0 || fi >= (int)mFiltered.size()) continue;
        int idx = mFiltered[fi];
        auto& item = mAllItems[idx];
        mTransferFilePaths.push_back(item.path);
        if (firstFileName.empty()) firstFileName = item.filename;
    }
    if (mTransferFilePaths.empty()) { mQRState = QRState::Error; return; }

    mQRFileName = std::to_string(mTransferFilePaths.size()) + " files";
    mQRItemIdx = -1;

    std::string ip = mFileServer.GetIPAddress();
    if (ip == "0.0.0.0") {
        mQRState = QRState::Error;
        return;
    }

    int port = 8080;
    mQRUrl = "http://" + ip + ":" + std::to_string(port) + "/";

    if (!mQRCode.Encode(mQRUrl)) {
        mQRState = QRState::Error;
        return;
    }

    if (mQRCodeTexture) { SDL_DestroyTexture(mQRCodeTexture); mQRCodeTexture = nullptr; }
    mQRCodeTexture = mQRCode.Render(Gfx::GetRenderer(), 12);
    if (!mQRCodeTexture) {
        mQRState = QRState::Error;
        return;
    }

    if (!mFileServer.Start(port)) {
        mQRState = QRState::Error;
        return;
    }

    mQRState = QRState::ShowQR;

    auto paths = mTransferFilePaths;
    std::thread srvThread([this, paths]() {
        mFileServer.ServeMulti(paths);
    });
    srvThread.detach();
}

void Album::StopQRTransfer() {
    mFileServer.Stop();
    if (mQRCodeTexture) { SDL_DestroyTexture(mQRCodeTexture); mQRCodeTexture = nullptr; }
    mQRState = QRState::Inactive;
    mQRItemIdx = -1;
    mTransferMode = TransferMode::None;
}

void Album::UpdateQRTransfer(const Input& input) {
    auto status = mFileServer.GetStatus();
    bool isMulti = (mTransferMode == TransferMode::Multi);

    if (!isMulti) {
        if (mQRState == QRState::ShowQR) {
            if (status == FileServer::Status::Done) {
                mQRState = QRState::Done;
            } else if (status == FileServer::Status::Error) {
                mQRState = QRState::Error;
            }
        } else if (mQRState == QRState::Transferring) {
            if (status == FileServer::Status::Done) {
                mQRState = QRState::Done;
            } else if (status == FileServer::Status::Error) {
                mQRState = QRState::Error;
            }
        }

        if (mQRState == QRState::ShowQR && !mFileServer.GetClientIP().empty() &&
            !mFileServer.IsTransferConfirmed() &&
            input.IsPressed(Input::BUTTON_A) && !mPointerConsumedClick) {
            mFileServer.ConfirmTransfer();
        }

        if (mQRState == QRState::ShowQR && status == FileServer::Status::Transferring) {
            mQRState = QRState::Transferring;
        }
    } else {
        if (mQRState == QRState::ShowQR) {
            if (status == FileServer::Status::Error) {
                mQRState = QRState::Error;
            } else if (status == FileServer::Status::Transferring) {
                mQRState = QRState::Transferring;
            } else if (status == FileServer::Status::Done) {
                mQRState = QRState::Done;
            }
        } else if (mQRState == QRState::Transferring) {
            if (status == FileServer::Status::Done) {
                mQRState = QRState::Done;
            } else if (status == FileServer::Status::Error) {
                mQRState = QRState::Error;
            }
        } else if (mQRState == QRState::Done) {
            if (status == FileServer::Status::Transferring) {
                mQRState = QRState::Transferring;
            } else if (status == FileServer::Status::Error) {
                mQRState = QRState::Error;
            }
        }

        if (mQRState == QRState::Transferring || mQRState == QRState::Done) {
            int idx = mFileServer.GetCurrentFileIdx();
            if (idx >= 0 && idx < (int)mTransferFilePaths.size()) {
                std::string p = mTransferFilePaths[idx];
                size_t pos = p.find_last_of("/\\");
                std::string fname = (pos != std::string::npos) ? p.substr(pos + 1) : p;
                if (mQRFileName != fname) mQRFileName = fname;
            }
        }
    }

    if (input.IsPressed(Input::BUTTON_B)) {
        StopQRTransfer();
        mSidebarFocus = true;
    }
}

void Album::DrawQRTransfer() {
    Gfx::DrawRectFilled(0, 0, Gfx::SCREEN_WIDTH, Gfx::SCREEN_HEIGHT, {0xff, 0xff, 0xff, 0xff});

    int cx = Gfx::SCREEN_WIDTH / 2;
    int cy = Gfx::SCREEN_HEIGHT / 2;

    Gfx::Print(cx, 60, 36, Gfx::COLOR_TEXT, "Transfer to Device", Gfx::ALIGN_CENTER);
    Gfx::Print(cx, 110, 24, Gfx::COLOR_TEXT_DIM, mQRFileName, Gfx::ALIGN_CENTER);

    if (mQRState == QRState::Error) {
        Gfx::Print(cx, cy, 36, Gfx::COLOR_DELETE,
                   "Error starting transfer.\nCheck network connection.",
                   Gfx::ALIGN_CENTER);
        Gfx::Print(cx, cy + 120, 26, Gfx::COLOR_TEXT_DIM,
                   "B: Back", Gfx::ALIGN_CENTER);
        return;
    }

    if (mQRCodeTexture && (mQRState == QRState::ShowQR || mQRState == QRState::Transferring)) {
        int qrPx = 0, qrPy = 0;
        SDL_QueryTexture(mQRCodeTexture, nullptr, nullptr, &qrPx, &qrPy);
        int qrX = cx - qrPx / 2;
        int qrY = cy - qrPy / 2 - 40;

        int shadowOff = 6;
        Gfx::DrawRectFilled(qrX + shadowOff, qrY + shadowOff, qrPx, qrPy, {0, 0, 0, 30});
        Gfx::DrawRectRounded(qrX - 8, qrY - 8, qrPx + 16, qrPy + 16, 8, {0xf5, 0xf5, 0xf5, 0xff});
        Gfx::DrawRectRoundedOutline(qrX - 8, qrY - 8, qrPx + 16, qrPy + 16, 8, Gfx::COLOR_ACCENT, 2);

        Gfx::DrawTexture(mQRCodeTexture, qrX, qrY, qrPx, qrPy);
    }

    Gfx::Print(cx, cy + 270, 20, Gfx::COLOR_ACCENT, mQRUrl, Gfx::ALIGN_CENTER);

    int statusY = cy + 320;
    switch (mQRState) {
        case QRState::ShowQR: {
            bool isMulti = (mTransferMode == TransferMode::Multi);
            std::string clientIP = mFileServer.GetClientIP();
            bool connected = !clientIP.empty();
            bool confirmed = mFileServer.IsTransferConfirmed();
            if (isMulti) {
                if (connected) {
                    Gfx::Print(cx, statusY, 28, Gfx::COLOR_ACCENT,
                               "Connected from: " + clientIP,
                               Gfx::ALIGN_CENTER);
                    Gfx::Print(cx, statusY + 40, 24, Gfx::COLOR_TEXT_DIM,
                               "Open the link on your device to download files",
                               Gfx::ALIGN_CENTER);
                } else {
                    Gfx::Print(cx, statusY, 28, Gfx::COLOR_TEXT,
                               "Scan the QR code with your smart device",
                               Gfx::ALIGN_CENTER);
                    Gfx::Print(cx, statusY + 40, 24, Gfx::COLOR_TEXT_DIM,
                               std::to_string(mTransferFilePaths.size()) + " file(s) ready to transfer",
                               Gfx::ALIGN_CENTER);
                }
            } else if (connected && confirmed) {
                Gfx::Print(cx, statusY, 28, Gfx::COLOR_TEXT,
                           "Waiting for device...",
                           Gfx::ALIGN_CENTER);
                Gfx::Print(cx, statusY + 40, 24, Gfx::COLOR_TEXT_DIM,
                           "The file transfer will begin shortly",
                           Gfx::ALIGN_CENTER);
            } else if (connected) {
                Gfx::Print(cx, statusY, 28, Gfx::COLOR_ACCENT,
                           "Connected from: " + clientIP,
                           Gfx::ALIGN_CENTER);
                Gfx::Print(cx, statusY + 40, 28, Gfx::COLOR_TEXT,
                           "Press A to start transfer",
                           Gfx::ALIGN_CENTER);
            } else {
                Gfx::Print(cx, statusY, 28, Gfx::COLOR_TEXT,
                           "Scan the QR code with your smart device",
                           Gfx::ALIGN_CENTER);
                Gfx::Print(cx, statusY + 40, 24, Gfx::COLOR_TEXT_DIM,
                           "Make sure your device is on the same network",
                           Gfx::ALIGN_CENTER);
            }
            break;
        }
        case QRState::Transferring: {
            int prog = mFileServer.GetProgress();
            int barW = 400, barH = 20;
            int barX = cx - barW / 2;
            int barY = statusY;
            Gfx::DrawRectRounded(barX, barY, barW, barH, 10, {0xe0, 0xe0, 0xe0, 0xff});
            Gfx::DrawRectRounded(barX, barY, barW * prog / 100, barH, 10, Gfx::COLOR_ACCENT);
            Gfx::Print(cx, barY + barH / 2, 22, Gfx::COLOR_WHITE,
                       std::to_string(prog) + "%", Gfx::ALIGN_CENTER);
            if (mTransferMode == TransferMode::Multi) {
                int cur = mFileServer.GetCurrentFileIdx() + 1;
                int total = mFileServer.GetTotalFiles();
                Gfx::Print(cx, barY + barH + 20, 22, Gfx::COLOR_TEXT,
                           "File " + std::to_string(cur) + " of " + std::to_string(total),
                           Gfx::ALIGN_CENTER);
                Gfx::Print(cx, barY + barH + 50, 26, Gfx::COLOR_TEXT,
                           "Transferring...", Gfx::ALIGN_CENTER);
            } else {
                Gfx::Print(cx, barY + barH + 36, 26, Gfx::COLOR_TEXT,
                           "Transferring...", Gfx::ALIGN_CENTER);
            }
            break;
        }
        case QRState::Done:
            Gfx::Print(cx, statusY, 32, SDL_Color{0x2e, 0x9a, 0x2e, 0xff},
                       "Transfer complete!", Gfx::ALIGN_CENTER);
            if (mTransferMode == TransferMode::Multi) {
                int served = mFileServer.GetServedCount();
                int total = mFileServer.GetTotalFiles();
                if (served >= total) {
                    Gfx::Print(cx, statusY + 50, 24, Gfx::COLOR_TEXT_DIM,
                               "All files transferred!   B: Back", Gfx::ALIGN_CENTER);
                } else {
                    Gfx::Print(cx, statusY + 50, 24, Gfx::COLOR_TEXT_DIM,
                               std::to_string(served) + " of " + std::to_string(total) + " files transferred   B: Back",
                               Gfx::ALIGN_CENTER);
                }
            } else {
                Gfx::Print(cx, statusY + 50, 24, Gfx::COLOR_TEXT_DIM,
                           "B: Back", Gfx::ALIGN_CENTER);
            }
            break;
        case QRState::Error:
            Gfx::Print(cx, statusY, 28, Gfx::COLOR_DELETE,
                       "Transfer failed", Gfx::ALIGN_CENTER);
            Gfx::Print(cx, statusY + 40, 24, Gfx::COLOR_TEXT_DIM,
                       "X: Retry   B: Back", Gfx::ALIGN_CENTER);
            break;
        default: break;
    }

    int hintY = Gfx::SCREEN_HEIGHT - 40;
    constexpr int IZ = 28;
    constexpr int LZ = 24;
    constexpr int IG = 8;
    bool isMulti = (mTransferMode == TransferMode::Multi);
    bool showA = (mQRState == QRState::ShowQR && !isMulti &&
                  !mFileServer.GetClientIP().empty() &&
                  !mFileServer.IsTransferConfirmed());
    if (showA) {
        Gfx::PrintIcon(350, hintY, IZ, Gfx::COLOR_BTN_A, Glyphs::A, Gfx::ALIGN_CENTER);
        Gfx::Print(350 + IZ / 2 + IG, hintY, LZ, Gfx::COLOR_TEXT, "Start",
                   Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);
        int bx = 472;
        Gfx::PrintIcon(bx, hintY, IZ, Gfx::COLOR_BTN_B, Glyphs::B, Gfx::ALIGN_CENTER);
        Gfx::Print(bx + IZ / 2 + IG, hintY, LZ, Gfx::COLOR_TEXT, "Cancel",
                   Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);
    } else {
        Gfx::PrintIcon(180, hintY, IZ, Gfx::COLOR_BTN_B, Glyphs::B, Gfx::ALIGN_CENTER);
        Gfx::Print(180 + IZ / 2 + IG, hintY, LZ, Gfx::COLOR_TEXT, "Back",
                   Gfx::ALIGN_LEFT | Gfx::ALIGN_VERTICAL);
    }

    if (mPointerDraw) {
        Gfx::DrawCircleFilled(mPointerScreenX, mPointerScreenY, 10, Gfx::COLOR_WHITE);
        Gfx::DrawCircleOutline(mPointerScreenX, mPointerScreenY, 12, Gfx::COLOR_BLACK, 2);
    }
}
