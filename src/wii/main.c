#include <stdlib.h>
#include <string.h>
#include <wiiuse/wpad.h>
#include <fat.h>
#include <ogc/audio.h>
#include <asndlib.h>
#include "grr_renderer.h"
#include "launcher.h"
#include "game.h"

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    WiiGX_initVideo();
    WiiGX_drawLoadingStatus(0, 10, "STARTING");

    WiiGX_drawLoadingStatus(1, 10, "INIT FAT");
    if (!fatInitDefault()) {
        const char* lines[] = {
            "fatInitDefault() failed - is your SD card inserted?",
            "PRESS HOME TO EXIT"
        };
        WiiGX_drawTextScreen("FATAL", lines, 2);
        while (SYS_MainLoop()) {
            WPAD_ScanPads();
            if (WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME) exit(1);
            VIDEO_WaitVSync();
        }
    }

    WiiGX_drawLoadingStatus(2, 10, "INIT INPUT");
    WPAD_Init();
    WPAD_SetDataFormat(WPAD_CHAN_0, WPAD_FMT_BTNS_ACC);

#if defined(BUTTERSCOTCH_WII_ENABLE_AUDIO)
    WiiGX_drawLoadingStatus(3, 10, "AUDIO INIT");
    AUDIO_Init(NULL);
    WiiGX_drawLoadingStatus(3, 10, "ASND INIT");
    ASND_Init();
    WiiGX_drawLoadingStatus(3, 10, "ASND START");
    ASND_Pause(0);
#else
    WiiGX_drawLoadingStatus(3, 10, "AUDIO DISABLED");
#endif

    WiiGX_drawLoadingStatus(4, 10, "SCAN STORAGE");
    WiiLaunchEntry entry;
    memset(&entry, 0, sizeof(entry));
    if (!WiiLauncher_selectGame(&entry)) {
        const char* lines[] = {
            "NO GAMES FOUND IN sd:/butterscotch OR usb:/butterscotch",
            "PRESS HOME TO EXIT"
        };
        WiiGX_drawTextScreen("FATAL", lines, 2);
        while (SYS_MainLoop()) {
            WPAD_ScanPads();
            if (WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME) exit(1);
            VIDEO_WaitVSync();
        }
    }

    int rc = WiiGame_run(&entry,
#if defined(BUTTERSCOTCH_WII_ENABLE_AUDIO)
        true
#else
        false
#endif
    );
    WiiLauncher_destroyEntry(&entry);
    WiiGX_shutdownVideo();
    return rc;
}
