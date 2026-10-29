// Nintendo Switch platform layer: process init/exit, paths, logging, fatal errors.
// Note: libnx and the game's PR/ultratypes.h define u64 differently, so this file
// must not include game headers.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <malloc.h>
#include <pthread.h>
#include <sys/stat.h>
#include <arpa/inet.h>

#include <switch.h>

#include "pc/platform.h"
#include "platform_switch.h"

#define SWITCH_LOG_FILE SWITCH_USER_DIR "/log.txt"
#define SWITCH_STATS_INTERVAL_SECONDS 10

extern int __system_argc;
extern char **__system_argv;

extern u32 gGlobalTimer; // game/display.h

static int sNxlinkSocket = -1;

// Applet mode (launched from the album) only gets a small slice of memory,
// not enough for the game. Ask the user to relaunch through title takeover.
static void switch_check_applet_mode(void) {
    AppletType type = appletGetAppletType();
    if (type == AppletType_Application || type == AppletType_SystemApplication) { return; }

    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    printf("\n  sm64coopdx is running in applet mode.\n\n");
    printf("  Applet mode does not have enough memory to run the game.\n");
    printf("  Launch it through title takeover instead: hold R while\n");
    printf("  starting any game, then open the Homebrew Menu.\n\n");
    printf("  Press + to exit, or A to continue anyway (the game may crash).\n");
    consoleUpdate(NULL);

    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 down = padGetButtonsDown(&pad);
        if (down & HidNpadButton_Plus) {
            consoleExit(NULL);
            exit(0);
        }
        if (down & HidNpadButton_A) { break; }
        consoleUpdate(NULL);
    }

    consoleExit(NULL);
}

static void switch_init_logging(void) {
    // nxlink (nxlink -s) sends stdout/stderr back to the PC; otherwise log to the SD card
    if (__nxlink_host.s_addr != 0) {
        sNxlinkSocket = nxlinkStdio();
    }
    if (sNxlinkSocket < 0) {
        // truncate once, then append from both streams so neither overwrites the other
        FILE *log = fopen(SWITCH_LOG_FILE, "w");
        if (log) { fclose(log); }
        freopen(SWITCH_LOG_FILE, "a", stdout);
        freopen(SWITCH_LOG_FILE, "a", stderr);
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
}

// periodically logs game frames and heap use; shows the game loop is alive even with no renderer
static void *switch_stats_thread(void *arg) {
    (void) arg;
    u32 lastTimer = gGlobalTimer;
    while (true) {
        sleep(SWITCH_STATS_INTERVAL_SECONDS);
        struct mallinfo mi = mallinfo();
        u32 timer = gGlobalTimer;
        printf("[switch] game frames: %u (+%u), heap used: %lu MiB\n",
               timer, timer - lastTimer, (unsigned long)(mi.uordblks / (1024 * 1024)));
        lastTimer = timer;
    }
    return NULL;
}

// called by libnx before main()
void userAppInit(void) {
    switch_check_applet_mode();

    socketInitializeDefault();
    romfsInit();

    mkdir("sdmc:/switch", 0777);
    mkdir(SWITCH_USER_DIR, 0777);

    switch_init_logging();

    appletSetFocusHandlingMode(AppletFocusHandlingMode_SuspendHomeSleep);

    u64 totalMem = 0;
    svcGetInfo(&totalMem, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    printf("sm64coopdx for Nintendo Switch: applet type %d, %lu MiB of memory, %s\n",
           (int)appletGetAppletType(), (unsigned long)(totalMem / (1024 * 1024)),
           envIsNso() ? "launched as NSO" : "launched by hbloader");

    pthread_t statsThread;
    if (pthread_create(&statsThread, NULL, switch_stats_thread, NULL) == 0) {
        pthread_detach(statsThread);
    }
}

// called by libnx after main() returns or exit()
void userAppExit(void) {
    fflush(stdout);
    fflush(stderr);
    if (sNxlinkSocket >= 0) {
        close(sNxlinkSocket);
        sNxlinkSocket = -1;
    }
    romfsExit();
    socketExit();
}

const char *sys_user_path(void) {
    return SWITCH_USER_DIR;
}

const char *sys_resource_path(void) {
    return SWITCH_RESOURCE_DIR;
}

const char *sys_exe_path_file(void) {
    // hbloader passes the NRO path as argv[0]; an installed NSP has no file path
    if (__system_argc > 0 && __system_argv && __system_argv[0] && !strncmp(__system_argv[0], "sdmc:", 5)) {
        return __system_argv[0];
    }
    return "";
}

const char *sys_exe_path_dir(void) {
    static char path[SYS_MAX_PATH];
    if ('\0' != path[0]) { return path; }

    const char *exeFilepath = sys_exe_path_file();
    const char *lastSeparator = strrchr(exeFilepath, '/');
    if (lastSeparator != NULL) {
        snprintf(path, sizeof(path), "%.*s", (int)(lastSeparator - exeFilepath), exeFilepath);
    } else {
        // the ROM scan iterates this directory, so it must exist
        snprintf(path, sizeof(path), "%s", SWITCH_USER_DIR);
    }

    return path;
}

bool switch_copy_file(const char *srcPath, const char *dstPath) {
    FILE *src = fopen(srcPath, "rb");
    if (!src) { return false; }
    FILE *dst = fopen(dstPath, "wb");
    if (!dst) {
        fclose(src);
        return false;
    }

    static char buffer[0x10000];
    bool ok = true;
    size_t read;
    while ((read = fread(buffer, 1, sizeof(buffer), src)) > 0) {
        if (fwrite(buffer, 1, read, dst) != read) {
            ok = false;
            break;
        }
    }
    if (ferror(src)) { ok = false; }

    fclose(src);
    if (fclose(dst) != 0) { ok = false; }
    if (!ok) { remove(dstPath); }
    return ok;
}

void switch_fatal(const char *msg) {
    fprintf(stderr, "FATAL ERROR:\n%s\n", msg);
    fflush(stderr);

    ErrorApplicationConfig config;
    if (R_SUCCEEDED(errorApplicationCreate(&config, "sm64coopdx encountered a fatal error.", msg))) {
        errorApplicationShow(&config);
    }

    exit(1);
}
