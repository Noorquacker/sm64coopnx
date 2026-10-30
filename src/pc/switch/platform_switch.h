#ifndef PLATFORM_SWITCH_H
#define PLATFORM_SWITCH_H

#include <stdbool.h>

#define SWITCH_USER_DIR "sdmc:/switch/sm64coopnx"
// user dir used before the project was renamed, moved to SWITCH_USER_DIR on first launch
#define SWITCH_LEGACY_USER_DIR "sdmc:/switch/sm64coopdx"
#define SWITCH_RESOURCE_DIR "romfs:"

#ifdef __cplusplus
extern "C" {
#endif

// std::filesystem::copy_file needs fchmod(), which libnx doesn't implement
bool switch_copy_file(const char *srcPath, const char *dstPath);

// debug: true once a "quit" file was created in the user dir (the window manager then quits the game)
bool switch_quit_requested(void);

// shows warnings from the stats thread (e.g. memory almost full) as popups; call on the main thread
void switch_show_pending_warnings(void);

// shows a system error dialog, then exits
void switch_fatal(const char *msg) __attribute__ ((noreturn));

#ifdef __cplusplus
}
#endif

#endif
