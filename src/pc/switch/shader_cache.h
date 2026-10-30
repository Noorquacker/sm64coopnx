#ifndef SHADER_CACHE_H
#define SHADER_CACHE_H

// DKSH shader cache layout, shared by the deko3d renderer and the host tool that
// prebuilds the romfs cache (tools/switch/shader_bake.c).
//
// Each color combiner program is stored as <key>_vsh.dksh and <key>_fsh.dksh, where the
// key hashes the generated GLSL plus SHADER_CACHE_VERSION. Combiners are recorded as one
// line per program: the five gfx_pc_precomp_shader() words (rgb1 alpha1 rgb2 alpha2 flags)
// followed by the 16 shader commands, which is all gfx_glsl_generate() reads.

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "pc/gfx/gfx_cc.h"

// bump when anything that ends up in the DKSH cache changes outside the GLSL source
#define SHADER_CACHE_VERSION "uam-v1.1.0"

static inline uint64_t shader_cache_fnv1a64(uint64_t hash, const char *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        hash ^= (uint8_t) data[i];
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

static inline uint64_t shader_cache_key(const char *vs, size_t vs_len, const char *fs, size_t fs_len) {
    uint64_t key = 0xcbf29ce484222325ULL;
    key = shader_cache_fnv1a64(key, SHADER_CACHE_VERSION, strlen(SHADER_CACHE_VERSION));
    key = shader_cache_fnv1a64(key, vs, vs_len);
    key = shader_cache_fnv1a64(key, fs, fs_len);
    return key;
}

#define SHADER_CACHE_RECORD_SIZE (5 * 9 + 2 * SHADER_CMD_LENGTH + 1)

// writes cc as a record line, without the newline
static inline void shader_cache_format_record(const struct ColorCombiner *cc, char out[SHADER_CACHE_RECORD_SIZE]) {
    int len = snprintf(out, SHADER_CACHE_RECORD_SIZE, "%08x %08x %08x %08x %08x ",
                       (unsigned) cc->cm.rgb1, (unsigned) cc->cm.alpha1, (unsigned) cc->cm.rgb2,
                       (unsigned) cc->cm.alpha2, (unsigned) cc->cm.flags);
    for (int i = 0; i < SHADER_CMD_LENGTH; i++) {
        len += snprintf(out + len, SHADER_CACHE_RECORD_SIZE - len, "%02x", cc->shader_commands[i]);
    }
}

// parses a record line into cc; returns 0 for blank lines and comments (#), -1 if malformed
static inline int shader_cache_parse_record(const char *line, struct ColorCombiner *cc) {
    while (*line == ' ' || *line == '\t') { line++; }
    if (*line == '\0' || *line == '\n' || *line == '\r' || *line == '#') { return 0; }

    unsigned words[5];
    char cmds[2 * SHADER_CMD_LENGTH + 1];
    if (sscanf(line, "%8x %8x %8x %8x %8x %32s", &words[0], &words[1], &words[2], &words[3], &words[4], cmds) != 6
        || strlen(cmds) != 2 * SHADER_CMD_LENGTH) {
        return -1;
    }
    memset(cc, 0, sizeof(*cc));
    cc->cm.rgb1 = words[0];
    cc->cm.alpha1 = words[1];
    cc->cm.rgb2 = words[2];
    cc->cm.alpha2 = words[3];
    cc->cm.flags = words[4];
    for (int i = 0; i < SHADER_CMD_LENGTH; i++) {
        unsigned v;
        if (sscanf(cmds + 2 * i, "%2x", &v) != 1) { return -1; }
        cc->shader_commands[i] = v;
    }
    return 1;
}

#endif
