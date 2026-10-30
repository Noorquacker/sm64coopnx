// Prebuilds the Switch DKSH shader cache on the build machine, so the game doesn't have to
// compile known color combiners at runtime (90-135 ms each on hardware).
//
// Reads combiner records (src/pc/switch/shader_cache.h), generates their GLSL 4.60 with the
// game's own generator (src/pc/gfx/gfx_glsl.c) and compiles them with devkitPro's host uam,
// the same uam version the game links.
//
// Usage: shader_bake UAM COMBINERS_TXT OUT_DIR

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "pc/gfx/gfx_glsl.h"
#include "pc/switch/shader_cache.h"

// gfx_cc.c's gfx_cc_precomp() calls into gfx_pc.c, which the tool doesn't link (and never calls)
void gfx_pc_precomp_shader(uint32_t rgb1, uint32_t alpha1, uint32_t rgb2, uint32_t alpha2, uint32_t flags) {
    (void) rgb1; (void) alpha1; (void) rgb2; (void) alpha2; (void) flags;
    abort();
}

static void write_file(const char *path, const char *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(data, 1, len, f) != len || fclose(f) != 0) {
        fprintf(stderr, "shader_bake: cannot write %s\n", path);
        exit(1);
    }
}

static void compile(const char *uam, const char *stage, const char *glsl, size_t len, const char *out) {
    char src[4096];
    snprintf(src, sizeof(src), "%s.glsl", out);
    write_file(src, glsl, len);

    pid_t pid = fork();
    if (pid == 0) {
        execl(uam, uam, "-s", stage, "-o", out, src, (char *) NULL);
        _exit(127);
    }
    int status;
    if (pid < 0 || waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "shader_bake: %s failed for %s:\n%.*s\n", uam, out, (int) len, glsl);
        exit(1);
    }
    remove(src);
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s UAM COMBINERS_TXT OUT_DIR\n", argv[0]);
        return 2;
    }
    const char *uam = argv[1];
    const char *outDir = argv[3];
    FILE *list = fopen(argv[2], "r");
    if (!list) {
        fprintf(stderr, "shader_bake: cannot open %s\n", argv[2]);
        return 1;
    }
    mkdir(outDir, 0777);

    static struct GfxGlslProgram glsl;
    char line[512];
    int lineNo = 0, baked = 0;
    while (fgets(line, sizeof(line), list)) {
        lineNo++;
        struct ColorCombiner cc;
        int r = shader_cache_parse_record(line, &cc);
        if (r == 0) { continue; }
        if (r < 0) {
            fprintf(stderr, "%s:%d: malformed combiner record\n", argv[2], lineNo);
            return 1;
        }

        gfx_glsl_generate(&cc, GFX_GLSL_460, &glsl);
        uint64_t key = shader_cache_key(glsl.vs, glsl.vs_len, glsl.fs, glsl.fs_len);
        char vsPath[4096], fsPath[4096];
        snprintf(vsPath, sizeof(vsPath), "%s/%016llx_vsh.dksh", outDir, (unsigned long long) key);
        snprintf(fsPath, sizeof(fsPath), "%s/%016llx_fsh.dksh", outDir, (unsigned long long) key);
        if (access(vsPath, F_OK) == 0 && access(fsPath, F_OK) == 0) { continue; } // duplicate record
        compile(uam, "vert", glsl.vs, glsl.vs_len, vsPath);
        compile(uam, "frag", glsl.fs, glsl.fs_len, fsPath);
        baked++;
    }
    fclose(list);
    printf("shader_bake: %d programs in %s\n", baked, outDir);
    return 0;
}
