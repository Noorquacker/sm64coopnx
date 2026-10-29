#ifndef PLATFORM_SWITCH_H
#define PLATFORM_SWITCH_H

#include <stdbool.h>

#define SWITCH_USER_DIR "sdmc:/switch/sm64coopdx"
#define SWITCH_RESOURCE_DIR "romfs:"

#ifdef __cplusplus
extern "C" {
#endif

// std::filesystem::copy_file needs fchmod(), which libnx doesn't implement
bool switch_copy_file(const char *srcPath, const char *dstPath);

// shows a system error dialog, then exits
void switch_fatal(const char *msg) __attribute__ ((noreturn));

#ifdef __cplusplus
}
#endif

#endif
