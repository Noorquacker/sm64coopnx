#ifndef UAM_H
#define UAM_H

// C interface to uam, the deko3d shader compiler (https://github.com/devkitPro/uam).
// libuam.a is built by tools/switch/build_uam.sh.

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

enum UamStage {
    UAM_STAGE_VERTEX,
    UAM_STAGE_FRAGMENT,
};

// Compiles GLSL 4.60 source and writes the DKSH binary to dksh_path.
// Compile errors are printed to stderr. Not thread safe.
bool uam_compile_dksh(enum UamStage stage, const char *glsl, const char *dksh_path);

#ifdef __cplusplus
}
#endif

#endif
