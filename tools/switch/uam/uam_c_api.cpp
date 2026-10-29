// C interface to uam's DekoCompiler, compiled into libuam.a by tools/switch/build_uam.sh

#include "compiler_iface.h"
#include "uam.h"

extern "C" bool uam_compile_dksh(enum UamStage stage, const char *glsl, const char *dksh_path) {
    DekoCompiler compiler{ stage == UAM_STAGE_VERTEX ? pipeline_stage_vertex : pipeline_stage_fragment };
    if (!compiler.CompileGlsl(glsl)) {
        return false;
    }
    compiler.OutputDksh(dksh_path);
    return true;
}
