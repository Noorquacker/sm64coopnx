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
#include "gfx_switch.h"

#define SWITCH_STATS_INTERVAL_SECONDS 10

extern int __system_argc;
extern char **__system_argv;

extern u32 gGlobalTimer; // game/display.h

static int sNxlinkSocket = -1;

// SWITCH_USER_DIR, or SWITCH_LEGACY_USER_DIR if it couldn't be moved
static const char *sUserDir = SWITCH_USER_DIR;

enum UserDirMigration { MIGRATION_NONE, MIGRATION_MOVED, MIGRATION_FAILED };

// Moves data from before the rename to sm64coopnx. If that fails (yuzu doesn't implement
// directory renames), keep using the old folder rather than starting over in a new one.
static enum UserDirMigration switch_init_user_dir(void) {
    enum UserDirMigration migration = MIGRATION_NONE;
    struct stat st;

    mkdir("sdmc:/switch", 0777);
    if (stat(SWITCH_USER_DIR, &st) != 0 && stat(SWITCH_LEGACY_USER_DIR, &st) == 0) {
        if (rename(SWITCH_LEGACY_USER_DIR, SWITCH_USER_DIR) == 0) {
            migration = MIGRATION_MOVED;
        } else {
            sUserDir = SWITCH_LEGACY_USER_DIR;
            migration = MIGRATION_FAILED;
        }
    }
    mkdir(sUserDir, 0777);
    return migration;
}

// Applet mode (launched from the album) only gets a small slice of memory,
// not enough for the game. Ask the user to relaunch through title takeover.
static void switch_check_applet_mode(void) {
    AppletType type = appletGetAppletType();
    if (type == AppletType_Application || type == AppletType_SystemApplication) { return; }

    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    printf("\n  sm64coopnx is running in applet mode.\n\n");
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
        char logPath[SYS_MAX_PATH];
        snprintf(logPath, sizeof(logPath), "%s/log.txt", sUserDir);
        FILE *log = fopen(logPath, "w");
        if (log) { fclose(log); }
        freopen(logPath, "a", stdout);
        freopen(logPath, "a", stderr);
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
}

static pthread_t sStatsThread;
static bool sStatsThreadRunning = false;
static volatile bool sStatsThreadStop = false;

// sleeps for a second in short steps, returns false once the thread should stop
static bool switch_stats_sleep(void) {
    for (int i = 0; i < 10; i++) {
        if (sStatsThreadStop) { return false; }
        usleep(100 * 1000);
    }
    return !sStatsThreadStop;
}

// periodically logs game ticks, presented frames per second and heap use,
// and checks every second for a framebuffer capture request
static void *switch_stats_thread(void *arg) {
    (void) arg;
    u32 lastTimer = gGlobalTimer;
    u32 lastPresents = gfx_deko3d_get_present_count();
    char captureTrigger[SYS_MAX_PATH];
    snprintf(captureTrigger, sizeof(captureTrigger), "%s/capture", sUserDir);
    for (u32 seconds = 1; switch_stats_sleep(); seconds++) {
        // commit the log file once a second, so the last lines survive a crash
        if (sNxlinkSocket < 0) {
            fflush(stdout);
            fsync(fileno(stdout));
        }

        // debug: creating an empty "capture" file in the user dir saves the next frame
        // (checked with stat first: yuzu's remove() reports success for missing files)
        struct stat st;
        if (stat(captureTrigger, &st) == 0 && remove(captureTrigger) == 0) {
            char path[SYS_MAX_PATH];
            snprintf(path, sizeof(path), "%s/capture_%u.ppm", sUserDir, gfx_deko3d_get_present_count());
            printf("[switch] capture requested at game tick %u\n", gGlobalTimer);
            gfx_deko3d_request_capture(path);
        }

        if (seconds % SWITCH_STATS_INTERVAL_SECONDS != 0) { continue; }
        struct mallinfo mi = mallinfo();
        u32 timer = gGlobalTimer;
        u32 presents = gfx_deko3d_get_present_count();
        printf("[switch] game ticks: %u (+%u), presented: %u (%.1f fps), heap used: %lu MiB\n",
               timer, timer - lastTimer, presents,
               (double) (presents - lastPresents) / SWITCH_STATS_INTERVAL_SECONDS,
               (unsigned long)(mi.uordblks / (1024 * 1024)));
        lastTimer = timer;
        lastPresents = presents;
    }
    return NULL;
}

// When the game quits back to hbmenu, the process keeps running hbmenu's code: a thread
// left behind would wake up in unloaded code and crash the console. Join it before exiting.
static void switch_stop_stats_thread(void) {
    if (!sStatsThreadRunning) { return; }
    sStatsThreadStop = true;
    pthread_join(sStatsThread, NULL);
    sStatsThreadRunning = false;
}

// Development aid: hbmenu and yuzu can't pass command line options, so if <user dir>/args.txt
// exists its whitespace separated contents replace the command line (e.g. "--server 7777").
// libnx calls main() with __system_argc/__system_argv after userAppInit().
static void switch_load_args_file(void) {
    char path[SYS_MAX_PATH];
    snprintf(path, sizeof(path), "%s/args.txt", sUserDir);
    FILE *f = fopen(path, "r");
    if (!f) { return; }

    static char contents[1024];
    size_t len = fread(contents, 1, sizeof(contents) - 1, f);
    fclose(f);
    contents[len] = '\0';

    static char *argv[64];
    int argc = 0;
    argv[argc++] = (__system_argc > 0 && __system_argv) ? __system_argv[0] : "sm64coopnx";
    for (char *token = strtok(contents, " \t\r\n"); token && argc < 63; token = strtok(NULL, " \t\r\n")) {
        argv[argc++] = token;
    }
    argv[argc] = NULL;

    __system_argc = argc;
    __system_argv = argv;
    printf("Using command line options from %s:", path);
    for (int i = 1; i < argc; i++) { printf(" %s", argv[i]); }
    printf("\n");
}

// called by libnx before main()
void userAppInit(void) {
    switch_check_applet_mode();

    socketInitializeDefault();
    romfsInit();

    enum UserDirMigration migration = switch_init_user_dir();
    switch_init_logging();

    appletSetFocusHandlingMode(AppletFocusHandlingMode_SuspendHomeSleep);

    u64 totalMem = 0;
    svcGetInfo(&totalMem, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    printf("sm64coopnx for Nintendo Switch: applet type %d, %lu MiB of memory, %s\n",
           (int)appletGetAppletType(), (unsigned long)(totalMem / (1024 * 1024)),
           envIsNso() ? "launched as NSO" : "launched by hbloader");
    if (migration == MIGRATION_MOVED) {
        printf("Moved user data from " SWITCH_LEGACY_USER_DIR " to " SWITCH_USER_DIR "\n");
    } else if (migration == MIGRATION_FAILED) {
        printf("Could not move " SWITCH_LEGACY_USER_DIR " to " SWITCH_USER_DIR ", still using the old folder\n");
    }
    switch_load_args_file();

    if (pthread_create(&sStatsThread, NULL, switch_stats_thread, NULL) == 0) {
        sStatsThreadRunning = true;
        // before stdio is closed (atexit handlers run first)
        atexit(switch_stop_stats_thread);
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
    return sUserDir;
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
        snprintf(path, sizeof(path), "%s", sUserDir);
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
    if (R_SUCCEEDED(errorApplicationCreate(&config, "sm64coopnx encountered a fatal error.", msg))) {
        errorApplicationShow(&config);
    }

    exit(1);
}
