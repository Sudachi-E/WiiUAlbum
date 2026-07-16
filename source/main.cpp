#include "ui/Gfx.hpp"
#include "ui/Input.hpp"
#include "album/Album.hpp"
#include "ui/Keyboard.hpp"

#include <SDL.h>
#include <whb/proc.h>
#include <whb/log.h>
#include <whb/log_udp.h>
#include <whb/sdcard.h>
#include <vpad/input.h>
#include <padscore/kpad.h>
#include <coreinit/title.h>
#include <coreinit/debug.h>
#include <sysapp/launch.h>
#include <nsysnet/netconfig.h>
#include <sndcore2/core.h>

#define LOG(fmt, ...) do { \
    OSReport("[ALBUM] " fmt "\n", ##__VA_ARGS__); \
    WHBLogPrintf("[ALBUM] " fmt, ##__VA_ARGS__); \
} while(0)

int main(int argc, char const* argv[]) {
    WHBProcInit();
    WHBLogUdpInit();

    LOG("=== WiiU Album STARTUP ===");

    AXInit();
    AXQuit();

    VPADInit();
    KPADInit();
    LOG("VPAD/KPAD init done");

    netconf_init();
    LOG("netconf_init done");

    bool sdMounted = WHBMountSdCard();
    if (!sdMounted) {
        LOG("WARNING: WHBMountSdCard() failed — SD card may be unavailable");
    } else {
        LOG("SD card mounted at: %s", WHBGetSdCardMountPath());
    }

    if (!Gfx::Init()) {
        LOG("ERROR: Gfx::Init failed");
        if (sdMounted) WHBUnmountSdCard();
        WHBProcShutdown();
        WHBLogUdpDeinit();
        return -1;
    }
    LOG("Gfx::Init done");

    Keyboard::Init();
    LOG("Keyboard::Init done");

    const char* sdRoot = sdMounted ? WHBGetSdCardMountPath() : "fs:/vol/external01";
    LOG("Using SD root: %s", sdRoot);

    {
        Input input;
        Album album(sdRoot);
        LOG("Entering main loop");

        while (WHBProcIsRunning()) {
            SDL_PumpEvents();

            Keyboard::Update();

            input.Update();
            album.Update(input);

            if (!WHBProcIsRunning()) break;

            album.Draw();
            Gfx::Render();
        }

        LOG("Main loop exited — shutting down");
        album.StopThumbWorkers();
    }

    Keyboard::Shutdown();
    LOG("Keyboard::Shutdown done");

    LOG("Calling Gfx::Shutdown");
    Gfx::Shutdown();

    if (sdMounted) WHBUnmountSdCard();

    KPADShutdown();
    netconf_close();
    VPADShutdown();

    LOG("Calling WHBProcShutdown");
    WHBProcShutdown();
    LOG("WiiU Album shutdown complete");
    WHBLogUdpDeinit();
    return 0;
}
