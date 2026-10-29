#ifndef GFX_GLSL_H
#define GFX_GLSL_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "gfx_cc.h"

// GLSL source generation for color combiner shaders, shared by the GLSL based backends.

enum GfxGlslDialect {
    GFX_GLSL_120,     // OpenGL 2.1
    GFX_GLSL_ES_100,  // OpenGL ES 2.0
    GFX_GLSL_460,     // explicit locations and bindings, uniforms in GFX_GLSL_460_UNIFORM_BLOCK (deko3d)
};

#define GFX_GLSL_MAX_ATTRIBS 7
#define GFX_GLSL_SOURCE_SIZE 8192

// In GFX_GLSL_460 every uniform lives in one std140 block with this fixed layout,
// bound at GFX_GLSL_460_UNIFORM_BINDING. Texture N uses sampler binding N.
#define GFX_GLSL_460_UNIFORM_BINDING 0
#define GFX_GLSL_460_UNIFORM_BLOCK \
    "layout(std140, binding = 0) uniform Uniforms {\n" \
    "    vec2 uTex0Size;\n" \
    "    vec2 uTex1Size;\n" \
    "    bool uTex0Filter;\n" \
    "    bool uTex1Filter;\n" \
    "    int uFilter;\n" \
    "    float uFrameCount;\n" \
    "    vec3 uLightmapColor;\n" \
    "    int uShaderFlags[8];\n" \
    "    float uShaderFlagValues[8];\n" \
    "};\n"

struct GfxGlslProgram {
    char vs[GFX_GLSL_SOURCE_SIZE];
    char fs[GFX_GLSL_SOURCE_SIZE];
    size_t vs_len;
    size_t fs_len;

    // vertex layout: attributes in order, interleaved floats (location = index in GFX_GLSL_460)
    uint8_t num_attribs;
    uint8_t num_floats;
    char attrib_names[GFX_GLSL_MAX_ATTRIBS][16];
    uint8_t attrib_sizes[GFX_GLSL_MAX_ATTRIBS];

    // which uniforms the shader uses
    uint8_t num_inputs;
    bool used_textures[2];
    bool used_noise;
    bool used_lightmap;
    bool world_geometry;
};

void gfx_glsl_generate(struct ColorCombiner *cc, enum GfxGlslDialect dialect, struct GfxGlslProgram *prg);

#endif
