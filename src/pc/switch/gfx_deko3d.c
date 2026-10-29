// deko3d rendering backend for Nintendo Switch.
//
// The device uses -1..1 depth like OpenGL, so Fast3D's depth values pass through
// unchanged. The origin is upper-left (clip y = +1 is the top of the screen, memory
// row 0 is the top row as scanned out), so Fast3D's bottom-up viewport and scissor
// rectangles are flipped against the output height. DkDeviceFlags_OriginLowerLeft
// would avoid the flip but renders upside down: like OpenGL textures, row 0 becomes
// the bottom, while the display shows row 0 at the top. (gl_FragCoord.y counts from
// the top too, which only shifts the phase of the dither/noise/scanline patterns.)
//
// Color combiner shaders come from gfx_glsl.c (GLSL 4.60), are compiled at runtime
// with uam and cached as DKSH files in <user dir>/shadercache.
//
// Memory: GPU memory that may still be read by a frame in flight (texture images,
// shader code, overflow blocks) is only freed after that frame's fence has passed.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <time.h>
#include <pthread.h>
#include <sys/stat.h>

#include <deko3d.h>
#include <uam.h>

#ifndef _LANGUAGE_C
# define _LANGUAGE_C
#endif
#include <PR/gbi.h>

#include "macros.h"
#include "pc/configfile.h"
#include "pc/platform.h"
#include "pc/gfx/gfx_cc.h"
#include "pc/gfx/gfx_glsl.h"
#include "pc/gfx/gfx_pc.h"
#include "gfx_switch.h"

#define FB_NUM 3
#define FB_WIDTH  SWITCH_DOCKED_WIDTH
#define FB_HEIGHT SWITCH_DOCKED_HEIGHT

#define FRAMES_IN_FLIGHT 2
#define CMD_MEM_PER_FRAME        (2 * 1024 * 1024)
#define TRANSIENT_MEM_PER_FRAME  (16 * 1024 * 1024) // vertices and texture upload staging
#define OVERFLOW_BLOCK_SIZE      (4 * 1024 * 1024)
#define CODE_MEM_SIZE            (4 * 1024 * 1024)
#define TEXTURE_CHUNK_SIZE       (32 * 1024 * 1024)
#define MAX_HEAP_CHUNKS          64

#define DEKO3D_MAX_TEXTURES 8192 // image descriptors; texture 0 is a 1x1 white fallback
#define NUM_WRAP_MODES 3
#define NUM_SAMPLERS (2 * NUM_WRAP_MODES * NUM_WRAP_MODES)

// bump when anything that ends up in the DKSH cache changes outside the GLSL source
#define SHADER_CACHE_VERSION "uam-v1.1.0"

#define ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((a) - 1))

  //////////////////
 // heap memory //
//////////////////

// First-fit allocator over one or more memory blocks, used for texture images and shader code.

struct HeapRange {
    uint32_t offset;
    uint32_t size;
};

struct HeapChunk {
    DkMemBlock mem;
    uint32_t size;
    struct HeapRange *free;
    uint32_t numFree;
    uint32_t capFree;
};

struct Heap {
    const char *name;
    uint32_t flags;
    uint32_t chunkSize;
    bool growable;
    struct HeapChunk chunks[MAX_HEAP_CHUNKS];
    uint32_t numChunks;
};

struct HeapAlloc {
    struct Heap *heap;
    uint16_t chunk;
    uint32_t offset;
    uint32_t size;
};

  ////////////
 // state //
////////////

struct ShaderProgram {
    uint64_t hash;
    DkShader shaders[2];
    struct HeapAlloc code[2];
    uint8_t num_inputs;
    bool used_textures[2];
    uint8_t num_attribs;
    uint8_t num_floats;
    DkVtxAttribState attribs[GFX_GLSL_MAX_ATTRIBS];
    bool valid;
};

struct Deko3dTexture {
    DkImage image;
    struct HeapAlloc alloc;
    uint32_t width;
    uint32_t height;
    uint8_t sampler;
    bool filter;
    bool valid;
};

// mirrors GFX_GLSL_460_UNIFORM_BLOCK (std140)
struct Uniforms {
    float tex0Size[2];
    float tex1Size[2];
    uint32_t tex0Filter;
    uint32_t tex1Filter;
    int32_t filter;
    float frameCount;
    float lightmapColor[4];
    int32_t shaderFlags[SHADER_FLAG_MAX][4];
    float shaderFlagValues[SHADER_FLAG_MAX][4];
};
_Static_assert(offsetof(struct Uniforms, lightmapColor) == 32, "std140 layout");
_Static_assert(offsetof(struct Uniforms, shaderFlags) == 48, "std140 layout");
_Static_assert(offsetof(struct Uniforms, shaderFlagValues) == 176, "std140 layout");

// memory and objects freed once a frame slot's fence has passed
struct FrameGarbage {
    struct HeapAlloc *allocs;
    uint32_t numAllocs;
    uint32_t capAllocs;
    DkMemBlock *blocks;
    uint32_t numBlocks;
    uint32_t capBlocks;
};

static void *sNativeWindow = NULL;
static uint32_t sOutputWidth = SWITCH_HANDHELD_WIDTH;
static uint32_t sOutputHeight = SWITCH_HANDHELD_HEIGHT;
static bool sOutputSizeChanged = true;

static DkDevice sDevice;
static DkQueue sQueue;
static DkMemBlock sImageMem;
static DkImage sFramebuffers[FB_NUM];
static DkImage sDepthBuffer;
static DkSwapchain sSwapchain;

static struct Heap sCodeHeap;
static struct Heap sTextureHeap;

// uniform buffer and descriptor sets
static DkMemBlock sStaticMem;
static DkGpuAddr sUniformGpu;
static DkGpuAddr sSamplerDescGpu;
static DkGpuAddr sImageDescGpu;

// per frame in flight: command memory, transient memory, fence, garbage
static DkMemBlock sCmdMem;
static DkCmdBuf sCmdBuf;
static DkMemBlock sTransientMem;
static uint32_t sTransientOffset = 0;
static DkMemBlock sOverflowBlock = NULL;
static uint32_t sOverflowOffset = 0;
static DkFence sFrameFences[FRAMES_IN_FLIGHT];
static struct FrameGarbage sGarbage[FRAMES_IN_FLIGHT];
static uint32_t sFrameSlot = 0;

static uint32_t sSwapInterval = UINT32_MAX;
static int sImageSlot = -1;
static bool sFrameActive = false;
static uint32_t sPresentCount = 0;
static uint32_t sFrameCount = 0;

static struct ShaderProgram sPrograms[CC_MAX_SHADERS];
static uint8_t sProgramPoolSize = 0;
static uint8_t sProgramPoolIndex = 0;
static struct ShaderProgram *sCurProgram = NULL;
static char sShaderCacheDir[SYS_MAX_PATH];

static struct Deko3dTexture *sTextures = NULL;
static uint32_t sNumTextures = 0;
static uint32_t sTileTexture[2] = { 0, 0 };
static int sCurTile = 0;

// shadowed render state, applied lazily before each draw
static bool sDepthTest = false;
static bool sDepthMask = false;
static bool sDecal = false;
static bool sUseAlpha = false;
static DkViewport sViewport;
static DkScissor sScissor;
static struct Uniforms sLastUniforms;

enum {
    DIRTY_SHADER    = 1 << 0,
    DIRTY_TEXTURES  = 1 << 1,
    DIRTY_DEPTH     = 1 << 2,
    DIRTY_RASTER    = 1 << 3,
    DIRTY_BLEND     = 1 << 4,
    DIRTY_VIEWPORT  = 1 << 5,
    DIRTY_SCISSOR   = 1 << 6,
    DIRTY_UNIFORMS  = 1 << 7,
    DIRTY_BARRIER   = 1 << 8, // texture uploads/descriptor updates since the last draw
    DIRTY_ALL       = 0xFF,
};
static uint32_t sDirty = DIRTY_ALL;

// framebuffer capture for debugging, see gfx_deko3d_request_capture()
static DkMemBlock sCaptureMem = NULL;
static char sCapturePath[256];
static volatile bool sCaptureRequested = false;

  /////////////////////
 // memory & setup //
/////////////////////

static void gfx_deko3d_debug_callback(UNUSED void *userData, const char *context, DkResult result, const char *message) {
    fprintf(stderr, "[deko3d] %s: %s (result %d)\n", context, message, (int) result);
    if (result != DkResult_Success) {
        sys_fatal("deko3d error in %s:\n%s", context, message);
    }
}

static DkMemBlock gfx_deko3d_memblock_create(uint32_t size, uint32_t flags) {
    DkMemBlockMaker maker;
    dkMemBlockMakerDefaults(&maker, sDevice, ALIGN_UP(size, DK_MEMBLOCK_ALIGNMENT));
    maker.flags = flags;
    return dkMemBlockCreate(&maker);
}

static void *grow_array(void *array, uint32_t *capacity, uint32_t count, size_t elementSize) {
    if (count < *capacity) { return array; }
    *capacity = *capacity ? *capacity * 2 : 16;
    array = realloc(array, *capacity * elementSize);
    if (!array) { sys_fatal("deko3d: out of memory"); }
    return array;
}

static void gfx_deko3d_garbage_add_block(DkMemBlock block) {
    struct FrameGarbage *g = &sGarbage[sFrameSlot];
    g->blocks = grow_array(g->blocks, &g->capBlocks, g->numBlocks, sizeof(DkMemBlock));
    g->blocks[g->numBlocks++] = block;
}

static void gfx_deko3d_cmdbuf_add_memory(UNUSED void *userData, DkCmdBuf cmdbuf, size_t minReqSize) {
    // this frame records more commands than its slice holds, give it another block
    uint32_t size = MAX(CMD_MEM_PER_FRAME, ALIGN_UP((uint32_t) minReqSize, DK_MEMBLOCK_ALIGNMENT));
    DkMemBlock block = gfx_deko3d_memblock_create(size, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
    dkCmdBufAddMemory(cmdbuf, block, 0, size);
    gfx_deko3d_garbage_add_block(block);
}

static void heap_init(struct Heap *heap, const char *name, uint32_t flags, uint32_t chunkSize, bool growable) {
    memset(heap, 0, sizeof(*heap));
    heap->name = name;
    heap->flags = flags;
    heap->chunkSize = chunkSize;
    heap->growable = growable;
}

static void heap_add_chunk(struct Heap *heap, uint32_t minSize) {
    if (heap->numChunks >= MAX_HEAP_CHUNKS) {
        sys_fatal("deko3d: %s memory exhausted (%u blocks)", heap->name, heap->numChunks);
    }
    struct HeapChunk *chunk = &heap->chunks[heap->numChunks++];
    chunk->size = ALIGN_UP(MAX(heap->chunkSize, minSize), DK_MEMBLOCK_ALIGNMENT);
    chunk->mem = gfx_deko3d_memblock_create(chunk->size, heap->flags);
    chunk->free = grow_array(NULL, &chunk->capFree, 0, sizeof(struct HeapRange));
    chunk->free[0] = (struct HeapRange) { 0, chunk->size };
    chunk->numFree = 1;
}

static bool heap_alloc(struct Heap *heap, uint32_t size, uint32_t align, struct HeapAlloc *out) {
    for (int attempt = 0; attempt < 2; attempt++) {
        for (uint32_t c = 0; c < heap->numChunks; c++) {
            struct HeapChunk *chunk = &heap->chunks[c];
            for (uint32_t i = 0; i < chunk->numFree; i++) {
                struct HeapRange *r = &chunk->free[i];
                uint32_t start = ALIGN_UP(r->offset, align);
                uint32_t end = r->offset + r->size;
                if (start + size > end) { continue; }

                uint32_t head = start - r->offset;
                uint32_t tail = end - (start + size);
                if (head > 0) {
                    r->size = head;
                    if (tail > 0) {
                        chunk->free = grow_array(chunk->free, &chunk->capFree, chunk->numFree, sizeof(struct HeapRange));
                        memmove(&chunk->free[i + 2], &chunk->free[i + 1], (chunk->numFree - i - 1) * sizeof(struct HeapRange));
                        chunk->free[i + 1] = (struct HeapRange) { start + size, tail };
                        chunk->numFree++;
                    }
                } else if (tail > 0) {
                    r->offset = start + size;
                    r->size = tail;
                } else {
                    memmove(&chunk->free[i], &chunk->free[i + 1], (chunk->numFree - i - 1) * sizeof(struct HeapRange));
                    chunk->numFree--;
                }
                *out = (struct HeapAlloc) { heap, c, start, size };
                return true;
            }
        }
        if (!heap->growable && heap->numChunks > 0) { return false; }
        heap_add_chunk(heap, size + align);
    }
    return false;
}

static void heap_free_now(const struct HeapAlloc *alloc) {
    struct HeapChunk *chunk = &alloc->heap->chunks[alloc->chunk];
    uint32_t i = 0;
    while (i < chunk->numFree && chunk->free[i].offset < alloc->offset) { i++; }

    chunk->free = grow_array(chunk->free, &chunk->capFree, chunk->numFree, sizeof(struct HeapRange));
    memmove(&chunk->free[i + 1], &chunk->free[i], (chunk->numFree - i) * sizeof(struct HeapRange));
    chunk->free[i] = (struct HeapRange) { alloc->offset, alloc->size };
    chunk->numFree++;

    // merge with the next and previous ranges
    if (i + 1 < chunk->numFree && chunk->free[i].offset + chunk->free[i].size == chunk->free[i + 1].offset) {
        chunk->free[i].size += chunk->free[i + 1].size;
        memmove(&chunk->free[i + 1], &chunk->free[i + 2], (chunk->numFree - i - 2) * sizeof(struct HeapRange));
        chunk->numFree--;
    }
    if (i > 0 && chunk->free[i - 1].offset + chunk->free[i - 1].size == chunk->free[i].offset) {
        chunk->free[i - 1].size += chunk->free[i].size;
        memmove(&chunk->free[i], &chunk->free[i + 1], (chunk->numFree - i - 1) * sizeof(struct HeapRange));
        chunk->numFree--;
    }
}

// frees after the frame currently being recorded has finished on the GPU
static void heap_free_deferred(const struct HeapAlloc *alloc) {
    if (!alloc->heap) { return; }
    struct FrameGarbage *g = &sGarbage[sFrameSlot];
    g->allocs = grow_array(g->allocs, &g->capAllocs, g->numAllocs, sizeof(struct HeapAlloc));
    g->allocs[g->numAllocs++] = *alloc;
}

static void *heap_cpu_addr(const struct HeapAlloc *alloc) {
    return (uint8_t *) dkMemBlockGetCpuAddr(alloc->heap->chunks[alloc->chunk].mem) + alloc->offset;
}

static void gfx_deko3d_collect_garbage(uint32_t slot) {
    struct FrameGarbage *g = &sGarbage[slot];
    for (uint32_t i = 0; i < g->numAllocs; i++) { heap_free_now(&g->allocs[i]); }
    for (uint32_t i = 0; i < g->numBlocks; i++) { dkMemBlockDestroy(g->blocks[i]); }
    g->numAllocs = 0;
    g->numBlocks = 0;
}

// CPU-written memory for this frame only (vertices, upload staging)
static void *gfx_deko3d_transient_alloc(uint32_t size, uint32_t align, DkGpuAddr *gpuAddr) {
    uint32_t offset = ALIGN_UP(sTransientOffset, align);
    if (offset + size <= TRANSIENT_MEM_PER_FRAME) {
        sTransientOffset = offset + size;
        uint32_t blockOffset = sFrameSlot * TRANSIENT_MEM_PER_FRAME + offset;
        *gpuAddr = dkMemBlockGetGpuAddr(sTransientMem) + blockOffset;
        return (uint8_t *) dkMemBlockGetCpuAddr(sTransientMem) + blockOffset;
    }

    // slice is full: continue in overflow blocks, freed with this frame
    offset = ALIGN_UP(sOverflowOffset, align);
    if (!sOverflowBlock || offset + size > OVERFLOW_BLOCK_SIZE) {
        uint32_t blockSize = MAX(OVERFLOW_BLOCK_SIZE, size);
        sOverflowBlock = gfx_deko3d_memblock_create(blockSize, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
        gfx_deko3d_garbage_add_block(sOverflowBlock);
        offset = 0;
    }
    sOverflowOffset = offset + size;
    *gpuAddr = dkMemBlockGetGpuAddr(sOverflowBlock) + offset;
    return (uint8_t *) dkMemBlockGetCpuAddr(sOverflowBlock) + offset;
}

static void gfx_deko3d_create_framebuffers(void) {
    DkImageLayoutMaker layoutMaker;
    dkImageLayoutMakerDefaults(&layoutMaker, sDevice);
    layoutMaker.dimensions[0] = FB_WIDTH;
    layoutMaker.dimensions[1] = FB_HEIGHT;

    layoutMaker.flags = DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_HwCompression;
    layoutMaker.format = DkImageFormat_RGBA8_Unorm;
    DkImageLayout colorLayout;
    dkImageLayoutInitialize(&colorLayout, &layoutMaker);

    layoutMaker.flags = DkImageFlags_UsageRender | DkImageFlags_HwCompression;
    layoutMaker.format = DkImageFormat_Z24S8;
    DkImageLayout depthLayout;
    dkImageLayoutInitialize(&depthLayout, &layoutMaker);

    // framebuffers followed by the depth buffer, all in one image memory block
    uint32_t colorSize = ALIGN_UP(dkImageLayoutGetSize(&colorLayout), dkImageLayoutGetAlignment(&colorLayout));
    uint32_t depthOffset = ALIGN_UP(FB_NUM * colorSize, dkImageLayoutGetAlignment(&depthLayout));
    uint32_t totalSize = depthOffset + dkImageLayoutGetSize(&depthLayout);
    sImageMem = gfx_deko3d_memblock_create(totalSize, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image);

    DkImage const *swapchainImages[FB_NUM];
    for (int i = 0; i < FB_NUM; i++) {
        dkImageInitialize(&sFramebuffers[i], &colorLayout, sImageMem, i * colorSize);
        swapchainImages[i] = &sFramebuffers[i];
    }
    dkImageInitialize(&sDepthBuffer, &depthLayout, sImageMem, depthOffset);

    DkSwapchainMaker swapchainMaker;
    dkSwapchainMakerDefaults(&swapchainMaker, sDevice, sNativeWindow, swapchainImages, FB_NUM);
    sSwapchain = dkSwapchainCreate(&swapchainMaker);
}

static uint32_t gfx_deko3d_wrap_index(uint32_t cm) {
    if (cm & G_TX_CLAMP) { return 2; }
    return (cm & G_TX_MIRROR) ? 1 : 0;
}

static uint8_t gfx_deko3d_sampler_index(bool linear, uint32_t cms, uint32_t cmt) {
    return (linear ? NUM_WRAP_MODES * NUM_WRAP_MODES : 0) + gfx_deko3d_wrap_index(cms) * NUM_WRAP_MODES + gfx_deko3d_wrap_index(cmt);
}

// static memory: uniform buffer, then sampler and image descriptor sets
static void gfx_deko3d_create_static_memory(void) {
    uint32_t uniformSize = ALIGN_UP(sizeof(struct Uniforms), DK_UNIFORM_BUF_ALIGNMENT);
    uint32_t samplerOffset = uniformSize;
    uint32_t imageOffset = ALIGN_UP(samplerOffset + NUM_SAMPLERS * sizeof(DkSamplerDescriptor), DK_IMAGE_DESCRIPTOR_ALIGNMENT);
    uint32_t totalSize = imageOffset + DEKO3D_MAX_TEXTURES * sizeof(DkImageDescriptor);

    sStaticMem = gfx_deko3d_memblock_create(totalSize, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
    DkGpuAddr base = dkMemBlockGetGpuAddr(sStaticMem);
    sUniformGpu = base;
    sSamplerDescGpu = base + samplerOffset;
    sImageDescGpu = base + imageOffset;

    // samplers never change, written once before any GPU use
    static const DkWrapMode wrapModes[NUM_WRAP_MODES] = { DkWrapMode_Repeat, DkWrapMode_MirroredRepeat, DkWrapMode_ClampToEdge };
    DkSamplerDescriptor *samplerDescs = (DkSamplerDescriptor *) ((uint8_t *) dkMemBlockGetCpuAddr(sStaticMem) + samplerOffset);
    for (int linear = 0; linear < 2; linear++) {
        for (uint32_t s = 0; s < NUM_WRAP_MODES; s++) {
            for (uint32_t t = 0; t < NUM_WRAP_MODES; t++) {
                DkSampler sampler;
                dkSamplerDefaults(&sampler);
                sampler.minFilter = linear ? DkFilter_Linear : DkFilter_Nearest;
                sampler.magFilter = linear ? DkFilter_Linear : DkFilter_Nearest;
                sampler.wrapMode[0] = wrapModes[s];
                sampler.wrapMode[1] = wrapModes[t];
                sampler.wrapMode[2] = DkWrapMode_ClampToEdge;
                uint8_t index = (linear ? NUM_WRAP_MODES * NUM_WRAP_MODES : 0) + s * NUM_WRAP_MODES + t;
                dkSamplerDescriptorInitialize(&samplerDescs[index], &sampler);
            }
        }
    }
}

  //////////////
 // shaders //
//////////////

static uint64_t fnv1a64(uint64_t hash, const char *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        hash ^= (uint8_t) data[i];
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

static double gfx_deko3d_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

// loads a DKSH file into code memory; returns false if it is missing or not a DKSH
static bool gfx_deko3d_load_dksh(const char *path, DkShader *shader, struct HeapAlloc *code) {
    FILE *f = fopen(path, "rb");
    if (!f) { return false; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);

    uint32_t magic = 0;
    if (size < 64 || fread(&magic, sizeof(magic), 1, f) != 1 || magic != 0x48534B44 /* "DKSH" */) {
        fclose(f);
        return false;
    }
    rewind(f);

    if (!heap_alloc(&sCodeHeap, size, DK_SHADER_CODE_ALIGNMENT, code)) {
        // shader code memory is fragmented or full: wait for the GPU and release what earlier frames freed
        // (not this frame's, its recorded commands may still use that code)
        dkQueueWaitIdle(sQueue);
        for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; i++) {
            if (i != sFrameSlot) { gfx_deko3d_collect_garbage(i); }
        }
        if (!heap_alloc(&sCodeHeap, size, DK_SHADER_CODE_ALIGNMENT, code)) {
            fclose(f);
            sys_fatal("deko3d: out of shader code memory");
        }
    }
    bool ok = fread(heap_cpu_addr(code), size, 1, f) == 1;
    fclose(f);
    if (!ok) {
        heap_free_now(code);
        return false;
    }

    DkShaderMaker maker;
    dkShaderMakerDefaults(&maker, code->heap->chunks[code->chunk].mem, code->offset);
    dkShaderInitialize(shader, &maker);
    return true;
}

static void gfx_deko3d_shader_paths(const struct GfxGlslProgram *glsl, char vsPath[SYS_MAX_PATH], char fsPath[SYS_MAX_PATH]) {
    uint64_t key = 0xcbf29ce484222325ULL;
    key = fnv1a64(key, SHADER_CACHE_VERSION, strlen(SHADER_CACHE_VERSION));
    key = fnv1a64(key, glsl->vs, glsl->vs_len);
    key = fnv1a64(key, glsl->fs, glsl->fs_len);
    snprintf(vsPath, SYS_MAX_PATH, "%s/%016llx_vsh.dksh", sShaderCacheDir, (unsigned long long) key);
    snprintf(fsPath, SYS_MAX_PATH, "%s/%016llx_fsh.dksh", sShaderCacheDir, (unsigned long long) key);
}

static void gfx_deko3d_compile_dksh(enum UamStage stage, const char *source, const char *path) {
    if (!uam_compile_dksh(stage, source, path)) {
        remove(path);
        fprintf(stderr, "[deko3d] shader compilation failed:\n%s\n", source);
        sys_fatal("deko3d: %s shader compilation failed (see log.txt)", stage == UAM_STAGE_VERTEX ? "vertex" : "fragment");
    }
}

static void gfx_deko3d_load_program(struct ShaderProgram *prg, const struct GfxGlslProgram *glsl) {
    char vsPath[SYS_MAX_PATH];
    char fsPath[SYS_MAX_PATH];
    gfx_deko3d_shader_paths(glsl, vsPath, fsPath);

    if (gfx_deko3d_load_dksh(vsPath, &prg->shaders[0], &prg->code[0])
        && gfx_deko3d_load_dksh(fsPath, &prg->shaders[1], &prg->code[1])) {
        return;
    }
    for (int i = 0; i < 2; i++) {
        if (prg->code[i].heap) { heap_free_now(&prg->code[i]); }
    }
    memset(prg->code, 0, sizeof(prg->code));

    // not cached (or the cache entry is damaged): compile with uam
    double start = gfx_deko3d_now();
    gfx_deko3d_compile_dksh(UAM_STAGE_VERTEX, glsl->vs, vsPath);
    gfx_deko3d_compile_dksh(UAM_STAGE_FRAGMENT, glsl->fs, fsPath);
    printf("[deko3d] compiled shader %016llx in %.0f ms\n", (unsigned long long) prg->hash, (gfx_deko3d_now() - start) * 1000.0);

    if (!gfx_deko3d_load_dksh(vsPath, &prg->shaders[0], &prg->code[0])
        || !gfx_deko3d_load_dksh(fsPath, &prg->shaders[1], &prg->code[1])) {
        sys_fatal("deko3d: could not load compiled shader %s", vsPath);
    }
}

  ////////////////////
 // render state //
////////////////////

static void gfx_deko3d_fill_uniforms(struct Uniforms *u) {
    memset(u, 0, sizeof(*u));
    for (int t = 0; t < 2; t++) {
        struct Deko3dTexture *tex = &sTextures[sTileTexture[t]];
        float *size = t ? u->tex1Size : u->tex0Size;
        size[0] = tex->width;
        size[1] = tex->height;
        if (t) { u->tex1Filter = tex->filter; } else { u->tex0Filter = tex->filter; }
    }
    u->filter = configFiltering;
    u->frameCount = (float) sFrameCount;
    u->lightmapColor[0] = gVertexColor[0] / 255.0f;
    u->lightmapColor[1] = gVertexColor[1] / 255.0f;
    u->lightmapColor[2] = gVertexColor[2] / 255.0f;
    for (int i = 0; i < SHADER_FLAG_MAX; i++) {
        u->shaderFlags[i][0] = gShaderFlags[i];
        u->shaderFlagValues[i][0] = gShaderFlagValues[i];
    }
}

static void gfx_deko3d_flush_state(void) {
    struct ShaderProgram *prg = sCurProgram;

    if (sDirty & DIRTY_BARRIER) {
        // finish texture copies and descriptor updates before sampling
        dkCmdBufBarrier(sCmdBuf, DkBarrier_Primitives, DkInvalidateFlags_Image | DkInvalidateFlags_Descriptors);
    }

    if (sDirty & DIRTY_SHADER) {
        DkShader const *shaders[] = { &prg->shaders[0], &prg->shaders[1] };
        DkVtxBufferState buffer = { prg->num_floats * sizeof(float), 0 };
        dkCmdBufBindShaders(sCmdBuf, DkStageFlag_GraphicsMask, shaders, 2);
        dkCmdBufBindVtxAttribState(sCmdBuf, prg->attribs, prg->num_attribs);
        dkCmdBufBindVtxBufferState(sCmdBuf, &buffer, 1);
    }

    if (sDirty & (DIRTY_SHADER | DIRTY_TEXTURES)) {
        DkResHandle handles[2];
        uint32_t count = 0;
        for (int t = 0; t < 2; t++) {
            struct Deko3dTexture *tex = &sTextures[sTileTexture[t]];
            uint32_t image = tex->valid ? sTileTexture[t] : 0;
            handles[count++] = dkMakeTextureHandle(image, tex->sampler);
        }
        dkCmdBufBindTextures(sCmdBuf, DkStage_Fragment, 0, handles, count);
    }

    if (sDirty & DIRTY_DEPTH) {
        DkDepthStencilState depth;
        dkDepthStencilStateDefaults(&depth);
        depth.depthTestEnable = sDepthTest;
        // like OpenGL, a disabled depth test also disables depth writes
        depth.depthWriteEnable = sDepthTest && sDepthMask;
        depth.depthCompareOp = DkCompareOp_Lequal;
        dkCmdBufBindDepthStencilState(sCmdBuf, &depth);
    }

    if (sDirty & DIRTY_RASTER) {
        DkRasterizerState raster;
        dkRasterizerStateDefaults(&raster);
        raster.cullMode = DkFace_None; // Fast3D culls on the CPU
        raster.depthBiasEnableMask = sDecal ? DkPolygonFlag_Fill : 0;
        dkCmdBufBindRasterizerState(sCmdBuf, &raster);
    }

    if (sDirty & DIRTY_BLEND) {
        DkColorState color;
        dkColorStateDefaults(&color);
        dkColorStateSetBlendEnable(&color, 0, sUseAlpha);
        dkCmdBufBindColorState(sCmdBuf, &color);
    }

    if (sDirty & DIRTY_VIEWPORT) {
        dkCmdBufSetViewports(sCmdBuf, 0, &sViewport, 1);
    }
    if (sDirty & DIRTY_SCISSOR) {
        dkCmdBufSetScissors(sCmdBuf, 0, &sScissor, 1);
    }

    struct Uniforms uniforms;
    gfx_deko3d_fill_uniforms(&uniforms);
    if ((sDirty & DIRTY_UNIFORMS) || memcmp(&uniforms, &sLastUniforms, sizeof(uniforms)) != 0) {
        dkCmdBufPushConstants(sCmdBuf, sUniformGpu, ALIGN_UP(sizeof(struct Uniforms), DK_UNIFORM_BUF_ALIGNMENT),
                              0, sizeof(uniforms), &uniforms);
        sLastUniforms = uniforms;
    }

    sDirty = 0;
}

// state that every new command list starts with
static void gfx_deko3d_bind_frame_state(void) {
    DkColorWriteState colorWrite;
    dkColorWriteStateDefaults(&colorWrite);
    dkCmdBufBindColorWriteState(sCmdBuf, &colorWrite);

    DkBlendState blend;
    dkBlendStateDefaults(&blend);
    // glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA) applies to color and alpha
    dkBlendStateSetFactors(&blend, DkBlendFactor_SrcAlpha, DkBlendFactor_InvSrcAlpha, DkBlendFactor_SrcAlpha, DkBlendFactor_InvSrcAlpha);
    dkCmdBufBindBlendState(sCmdBuf, 0, &blend);

    // glPolygonOffset(-2, -2); NVIDIA hardware takes twice the GL units (as in Mesa's nouveau)
    dkCmdBufSetDepthBias(sCmdBuf, -4.0f, 0.0f, -2.0f);

    dkCmdBufBindUniformBuffer(sCmdBuf, DkStage_Fragment, GFX_GLSL_460_UNIFORM_BINDING, sUniformGpu,
                              ALIGN_UP(sizeof(struct Uniforms), DK_UNIFORM_BUF_ALIGNMENT));
    dkCmdBufBindSamplerDescriptorSet(sCmdBuf, sSamplerDescGpu, NUM_SAMPLERS);
    dkCmdBufBindImageDescriptorSet(sCmdBuf, sImageDescGpu, DEKO3D_MAX_TEXTURES);

    sDirty = DIRTY_ALL;
}

  ////////////////////////////
 // framebuffer capture //
////////////////////////////

struct CaptureJob {
    char path[256];
    uint32_t width;
    uint32_t height;
    uint8_t *rgb;
};

// records a copy of the visible region; call before the frame's fence is signaled
static void gfx_deko3d_record_capture(void) {
    if (!sCaptureMem) {
        // CPU cached: reading back from uncached memory stalled the frame for seconds on hardware
        sCaptureMem = gfx_deko3d_memblock_create(FB_WIDTH * FB_HEIGHT * 4, DkMemBlockFlags_CpuCached | DkMemBlockFlags_GpuCached);
    }

    DkImageView view;
    dkImageViewDefaults(&view, &sFramebuffers[sImageSlot]);
    DkImageRect rect = { .x = 0, .y = 0, .z = 0, .width = sOutputWidth, .height = sOutputHeight, .depth = 1 };
    DkCopyBuf dst = { .addr = dkMemBlockGetGpuAddr(sCaptureMem), .rowLength = 0, .imageHeight = 0 };

    dkCmdBufBarrier(sCmdBuf, DkBarrier_Fragments, 0);
    dkCmdBufCopyImageToBuffer(sCmdBuf, &view, &rect, &dst, 0);
}

static void *gfx_deko3d_capture_writer(void *arg) {
    struct CaptureJob *job = arg;
    FILE *f = fopen(job->path, "wb");
    if (f) {
        fprintf(f, "P6\n%u %u\n255\n", job->width, job->height);
        fwrite(job->rgb, 3, job->width * job->height, f);
        fclose(f);
        printf("[deko3d] captured %ux%u frame to %s\n", job->width, job->height, job->path);
    } else {
        fprintf(stderr, "[deko3d] could not write capture %s\n", job->path);
    }
    free(job->rgb);
    free(job);
    return NULL;
}

// converts the captured pixels to RGB and writes them on a background thread;
// call after the frame's fence completed
static void gfx_deko3d_write_capture(void) {
    uint32_t width = sOutputWidth;
    uint32_t height = sOutputHeight;
    struct CaptureJob *job = malloc(sizeof(struct CaptureJob));
    uint8_t *rgb = malloc(width * height * 3);
    if (!job || !rgb) {
        free(job);
        free(rgb);
        return;
    }

    dkMemBlockFlushCpuCache(sCaptureMem, 0, width * height * 4);
    const uint8_t *pixels = dkMemBlockGetCpuAddr(sCaptureMem);
    for (uint32_t i = 0; i < width * height; i++) {
        memcpy(&rgb[i * 3], &pixels[i * 4], 3);
    }

    snprintf(job->path, sizeof(job->path), "%s", sCapturePath);
    job->width = width;
    job->height = height;
    job->rgb = rgb;

    pthread_t thread;
    if (pthread_create(&thread, NULL, gfx_deko3d_capture_writer, job) == 0) {
        pthread_detach(thread);
    } else {
        gfx_deko3d_capture_writer(job);
    }
}

void gfx_deko3d_request_capture(const char *path) {
    snprintf(sCapturePath, sizeof(sCapturePath), "%s", path);
    __atomic_store_n(&sCaptureRequested, true, __ATOMIC_RELEASE);
}

  //////////////////////////
 // window manager glue //
//////////////////////////

void gfx_deko3d_set_native_window(void *nwindow) {
    sNativeWindow = nwindow;
}

void gfx_deko3d_set_output_size(uint32_t width, uint32_t height) {
    sOutputWidth = MIN(width, FB_WIDTH);
    sOutputHeight = MIN(height, FB_HEIGHT);
    sOutputSizeChanged = true;
}

void gfx_deko3d_present(void) {
    if (sImageSlot < 0) { return; }
    dkQueuePresentImage(sQueue, sSwapchain, sImageSlot);
    sImageSlot = -1;
    sPresentCount++;
}

uint32_t gfx_deko3d_get_present_count(void) {
    return sPresentCount;
}

  /////////////////////////
 // GfxRenderingAPI //
/////////////////////////

static bool gfx_deko3d_z_is_from_0_to_1(void) {
    return false;
}

static void gfx_deko3d_unload_shader(struct ShaderProgram *old_prg) {
    if (old_prg == NULL || old_prg == sCurProgram) {
        sCurProgram = NULL;
    }
}

static void gfx_deko3d_load_shader(struct ShaderProgram *new_prg) {
    sCurProgram = new_prg;
    sDirty |= DIRTY_SHADER;
}

static struct ShaderProgram *gfx_deko3d_create_and_load_new_shader(struct ColorCombiner *cc) {
    static struct GfxGlslProgram glsl;
    gfx_glsl_generate(cc, GFX_GLSL_460, &glsl);

    struct ShaderProgram *prg = &sPrograms[sProgramPoolIndex];
    sProgramPoolIndex = (sProgramPoolIndex + 1) % CC_MAX_SHADERS;
    if (sProgramPoolSize < CC_MAX_SHADERS) { sProgramPoolSize++; }

    // the pool is a ring: release the code of the program being replaced once the GPU is done with it
    if (prg->valid) {
        heap_free_deferred(&prg->code[0]);
        heap_free_deferred(&prg->code[1]);
    }
    memset(prg, 0, sizeof(*prg));
    prg->hash = cc->hash;

    gfx_deko3d_load_program(prg, &glsl);

    static const DkVtxAttribSize sizes[] = { 0, DkVtxAttribSize_1x32, DkVtxAttribSize_2x32, DkVtxAttribSize_3x32, DkVtxAttribSize_4x32 };
    uint32_t offset = 0;
    for (int i = 0; i < glsl.num_attribs; i++) {
        prg->attribs[i] = (DkVtxAttribState) {
            .bufferId = 0, .isFixed = 0, .offset = offset,
            .size = sizes[glsl.attrib_sizes[i]], .type = DkVtxAttribType_Float, .isBgra = 0,
        };
        offset += glsl.attrib_sizes[i] * sizeof(float);
    }
    prg->num_attribs = glsl.num_attribs;
    prg->num_floats = glsl.num_floats;
    prg->num_inputs = glsl.num_inputs;
    prg->used_textures[0] = glsl.used_textures[0];
    prg->used_textures[1] = glsl.used_textures[1];
    prg->valid = true;

    gfx_deko3d_load_shader(prg);
    return prg;
}

static struct ShaderProgram *gfx_deko3d_lookup_shader(struct ColorCombiner *cc) {
    for (size_t i = 0; i < sProgramPoolSize; i++) {
        if (sPrograms[i].valid && sPrograms[i].hash == cc->hash) {
            return &sPrograms[i];
        }
    }
    return NULL;
}

static void gfx_deko3d_shader_get_info(struct ShaderProgram *prg, uint8_t *num_inputs, bool used_textures[2]) {
    *num_inputs = prg->num_inputs;
    used_textures[0] = prg->used_textures[0];
    used_textures[1] = prg->used_textures[1];
}

static uint32_t gfx_deko3d_new_texture(void) {
    if (sNumTextures >= DEKO3D_MAX_TEXTURES) {
        sys_fatal("deko3d: too many textures (%u)", DEKO3D_MAX_TEXTURES);
    }
    memset(&sTextures[sNumTextures], 0, sizeof(struct Deko3dTexture));
    return sNumTextures++;
}

static void gfx_deko3d_select_texture(int tile, uint32_t texture_id) {
    sTileTexture[tile] = texture_id;
    sCurTile = tile;
    sDirty |= DIRTY_TEXTURES;
}

// uploads into a texture id; an id that already has an image gets a new one (the old is freed after the GPU is done)
static void gfx_deko3d_upload_texture_to(uint32_t id, const uint8_t *rgba32_buf, int width, int height) {
    struct Deko3dTexture *tex = &sTextures[id];
    if (tex->valid) {
        heap_free_deferred(&tex->alloc);
    }

    DkImageLayoutMaker layoutMaker;
    dkImageLayoutMakerDefaults(&layoutMaker, sDevice);
    layoutMaker.format = DkImageFormat_RGBA8_Unorm;
    layoutMaker.dimensions[0] = width;
    layoutMaker.dimensions[1] = height;
    DkImageLayout layout;
    dkImageLayoutInitialize(&layout, &layoutMaker);

    if (!heap_alloc(&sTextureHeap, dkImageLayoutGetSize(&layout), dkImageLayoutGetAlignment(&layout), &tex->alloc)) {
        sys_fatal("deko3d: out of texture memory");
    }
    dkImageInitialize(&tex->image, &layout, sTextureHeap.chunks[tex->alloc.chunk].mem, tex->alloc.offset);

    DkGpuAddr stagingGpu;
    uint32_t size = width * height * 4;
    memcpy(gfx_deko3d_transient_alloc(size, 256, &stagingGpu), rgba32_buf, size);

    DkImageView view;
    dkImageViewDefaults(&view, &tex->image);
    DkCopyBuf src = { .addr = stagingGpu, .rowLength = 0, .imageHeight = 0 };
    DkImageRect rect = { .x = 0, .y = 0, .z = 0, .width = width, .height = height, .depth = 1 };
    dkCmdBufCopyBufferToImage(sCmdBuf, &src, &view, &rect, 0);

    // descriptor written on the GPU timeline, so draws already recorded keep the old image
    DkImageDescriptor desc;
    dkImageDescriptorInitialize(&desc, &view, false, false);
    dkCmdBufPushData(sCmdBuf, sImageDescGpu + id * sizeof(DkImageDescriptor), &desc, sizeof(desc));

    tex->width = width;
    tex->height = height;
    tex->valid = true;
    sDirty |= DIRTY_BARRIER | DIRTY_TEXTURES;
}

static void gfx_deko3d_upload_texture(const uint8_t *rgba32_buf, int width, int height) {
    if (!sFrameActive) {
        fprintf(stderr, "[deko3d] texture upload outside of a frame ignored\n");
        return;
    }
    gfx_deko3d_upload_texture_to(sTileTexture[sCurTile], rgba32_buf, width, height);
}

// like OpenGL, sampler parameters belong to the texture bound to the tile
static void gfx_deko3d_set_sampler_parameters(int tile, bool linear_filter, uint32_t cms, uint32_t cmt) {
    struct Deko3dTexture *tex = &sTextures[sTileTexture[tile]];
    tex->sampler = gfx_deko3d_sampler_index(linear_filter, cms, cmt);
    tex->filter = linear_filter;
    sCurTile = tile;
    sDirty |= DIRTY_TEXTURES;
}

static void gfx_deko3d_set_depth_test(bool depth_test) {
    sDepthTest = depth_test;
    sDirty |= DIRTY_DEPTH;
}

static void gfx_deko3d_set_depth_mask(bool z_upd) {
    sDepthMask = z_upd;
    sDirty |= DIRTY_DEPTH;
}

static void gfx_deko3d_set_zmode_decal(bool zmode_decal) {
    sDecal = zmode_decal;
    sDirty |= DIRTY_RASTER;
}

// Fast3D rectangles are OpenGL style (origin bottom-left), deko3d's origin is top-left
static void gfx_deko3d_set_viewport(int x, int y, int width, int height) {
    sViewport = (DkViewport) { (float) x, (float) ((int) sOutputHeight - y - height), (float) width, (float) height, 0.0f, 1.0f };
    sDirty |= DIRTY_VIEWPORT;
}

static void gfx_deko3d_set_scissor(int x, int y, int width, int height) {
    int top = (int) sOutputHeight - y - height;
    int x0 = MAX(x, 0);
    int y0 = MAX(top, 0);
    int x1 = MAX(MIN(x + width, (int) sOutputWidth), x0);
    int y1 = MAX(MIN(top + height, (int) sOutputHeight), y0);
    sScissor = (DkScissor) { x0, y0, x1 - x0, y1 - y0 };
    sDirty |= DIRTY_SCISSOR;
}

static void gfx_deko3d_set_use_alpha(bool use_alpha) {
    sUseAlpha = use_alpha;
    sDirty |= DIRTY_BLEND;
}

static void gfx_deko3d_draw_triangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris) {
    if (!sFrameActive || !sCurProgram) { return; }
    gfx_deko3d_flush_state();

    DkGpuAddr gpuAddr;
    uint32_t size = buf_vbo_len * sizeof(float);
    memcpy(gfx_deko3d_transient_alloc(size, 16, &gpuAddr), buf_vbo, size);
    dkCmdBufBindVtxBuffer(sCmdBuf, 0, gpuAddr, size);
    dkCmdBufDraw(sCmdBuf, DkPrimitive_Triangles, 3 * buf_vbo_num_tris, 1, 0, 0);
}

// starts recording into this frame slot's command memory
static void gfx_deko3d_begin_commands(void) {
    dkCmdBufClear(sCmdBuf);
    dkCmdBufAddMemory(sCmdBuf, sCmdMem, sFrameSlot * CMD_MEM_PER_FRAME, CMD_MEM_PER_FRAME);
    sTransientOffset = 0;
    sOverflowBlock = NULL;
    sOverflowOffset = 0;
}

static void gfx_deko3d_init(void) {
    if (!sNativeWindow) {
        sys_fatal("deko3d: no native window, the Switch window manager must be initialized first");
    }

    DkDeviceMaker deviceMaker;
    dkDeviceMakerDefaults(&deviceMaker);
    deviceMaker.cbDebug = gfx_deko3d_debug_callback;
    deviceMaker.flags = DkDeviceFlags_DepthMinusOneToOne | DkDeviceFlags_OriginUpperLeft;
    sDevice = dkDeviceCreate(&deviceMaker);

    gfx_deko3d_create_framebuffers();
    gfx_deko3d_create_static_memory();

    heap_init(&sCodeHeap, "shader code", DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code,
              CODE_MEM_SIZE + DK_SHADER_CODE_UNUSABLE_SIZE, false);
    heap_add_chunk(&sCodeHeap, 0);
    sCodeHeap.chunks[0].free[0].size -= DK_SHADER_CODE_UNUSABLE_SIZE; // the end of a code block can't hold code
    heap_init(&sTextureHeap, "texture", DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image, TEXTURE_CHUNK_SIZE, true);

    sCmdMem = gfx_deko3d_memblock_create(FRAMES_IN_FLIGHT * CMD_MEM_PER_FRAME, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
    sTransientMem = gfx_deko3d_memblock_create(FRAMES_IN_FLIGHT * TRANSIENT_MEM_PER_FRAME, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);

    DkCmdBufMaker cmdBufMaker;
    dkCmdBufMakerDefaults(&cmdBufMaker, sDevice);
    cmdBufMaker.cbAddMem = gfx_deko3d_cmdbuf_add_memory;
    sCmdBuf = dkCmdBufCreate(&cmdBufMaker);

    DkQueueMaker queueMaker;
    dkQueueMakerDefaults(&queueMaker, sDevice);
    queueMaker.flags = DkQueueFlags_Graphics;
    sQueue = dkQueueCreate(&queueMaker);

    snprintf(sShaderCacheDir, sizeof(sShaderCacheDir), "%s/shadercache", sys_user_path());
    mkdir(sShaderCacheDir, 0777);

    // texture 0: 1x1 white, sampled by tiles that have no texture yet
    sTextures = calloc(DEKO3D_MAX_TEXTURES, sizeof(struct Deko3dTexture));
    if (!sTextures) { sys_fatal("deko3d: out of memory"); }
    sNumTextures = 1;
    static const uint8_t white[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
    gfx_deko3d_begin_commands();
    gfx_deko3d_upload_texture_to(0, white, 1, 1);
    dkQueueSubmitCommands(sQueue, dkCmdBufFinishList(sCmdBuf));
    dkQueueWaitIdle(sQueue);

    printf("[deko3d] initialized: %d framebuffers of %dx%d, %d frames in flight\n", FB_NUM, FB_WIDTH, FB_HEIGHT, FRAMES_IN_FLIGHT);
}

static void gfx_deko3d_on_resize(void) {
}

static void gfx_deko3d_start_frame(void) {
    // wait until the GPU is done with this slot's memory, then release what that frame freed
    dkFenceWait(&sFrameFences[sFrameSlot], -1);
    gfx_deko3d_collect_garbage(sFrameSlot);

    if (sImageSlot < 0) {
        if (sOutputSizeChanged) {
            // render into the top-left corner and present only that region
            dkSwapchainSetCrop(sSwapchain, 0, 0, sOutputWidth, sOutputHeight);
            sOutputSizeChanged = false;
        }
        uint32_t swapInterval = configWindow.vsync ? 1 : 0;
        if (swapInterval != sSwapInterval) {
            dkSwapchainSetSwapInterval(sSwapchain, swapInterval);
            sSwapInterval = swapInterval;
        }

        sImageSlot = dkQueueAcquireImage(sQueue, sSwapchain);
    }
    // else: a frame was started but never presented (rendering_init() does this),
    // discard what it recorded and start over in the same image

    gfx_deko3d_begin_commands();
    sFrameCount++;

    DkImageView colorView;
    DkImageView depthView;
    dkImageViewDefaults(&colorView, &sFramebuffers[sImageSlot]);
    dkImageViewDefaults(&depthView, &sDepthBuffer);
    dkCmdBufBindRenderTarget(sCmdBuf, &colorView, &depthView);

    DkViewport viewport = { 0.0f, 0.0f, (float) sOutputWidth, (float) sOutputHeight, 0.0f, 1.0f };
    DkScissor scissor = { 0, 0, sOutputWidth, sOutputHeight };
    dkCmdBufSetViewports(sCmdBuf, 0, &viewport, 1);
    dkCmdBufSetScissors(sCmdBuf, 0, &scissor, 1);

    // depth writes must be enabled for the depth clear, as in the GL backend
    DkDepthStencilState depth;
    dkDepthStencilStateDefaults(&depth);
    depth.depthWriteEnable = true;
    dkCmdBufBindDepthStencilState(sCmdBuf, &depth);
    dkCmdBufClearColorFloat(sCmdBuf, 0, DkColorMask_RGBA, 0.0f, 0.0f, 0.0f, 1.0f);
    dkCmdBufClearDepthStencil(sCmdBuf, true, 1.0f, 0xFF, 0);

    gfx_deko3d_bind_frame_state();
    sFrameActive = true;
}

static void gfx_deko3d_end_frame(void) {
    if (!sFrameActive) { return; }

    bool capture = __atomic_load_n(&sCaptureRequested, __ATOMIC_ACQUIRE);
    if (capture) { gfx_deko3d_record_capture(); }

    DkFence *fence = &sFrameFences[sFrameSlot];
    dkCmdBufSignalFence(sCmdBuf, fence, capture); // flush so the CPU sees the captured pixels
    dkQueueSubmitCommands(sQueue, dkCmdBufFinishList(sCmdBuf));
    sFrameSlot = (sFrameSlot + 1) % FRAMES_IN_FLIGHT;
    sFrameActive = false;

    if (capture) {
        dkQueueFlush(sQueue);
        dkFenceWait(fence, -1);
        gfx_deko3d_write_capture();
        __atomic_store_n(&sCaptureRequested, false, __ATOMIC_RELEASE);
    }
}

static void gfx_deko3d_finish_render(void) {
}

static const char *gfx_deko3d_get_name(void) {
    return "deko3d";
}

static void heap_destroy(struct Heap *heap) {
    for (uint32_t i = 0; i < heap->numChunks; i++) {
        dkMemBlockDestroy(heap->chunks[i].mem);
        free(heap->chunks[i].free);
    }
    heap->numChunks = 0;
}

static void gfx_deko3d_shutdown(void) {
    if (!sDevice) { return; }

    // the game can quit in the middle of a frame: finish it so no swapchain image stays acquired
    if (sFrameActive) {
        dkQueueSubmitCommands(sQueue, dkCmdBufFinishList(sCmdBuf));
        sFrameActive = false;
    }
    gfx_deko3d_present();
    dkQueueWaitIdle(sQueue);

    // every memory block must be destroyed before the device (the validating library checks)
    for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; i++) {
        gfx_deko3d_collect_garbage(i);
        free(sGarbage[i].allocs);
        free(sGarbage[i].blocks);
        memset(&sGarbage[i], 0, sizeof(sGarbage[i]));
    }
    heap_destroy(&sTextureHeap);
    heap_destroy(&sCodeHeap);
    if (sCaptureMem) {
        dkMemBlockDestroy(sCaptureMem);
        sCaptureMem = NULL;
    }
    free(sTextures);
    sTextures = NULL;

    dkQueueDestroy(sQueue);
    dkCmdBufDestroy(sCmdBuf);
    dkMemBlockDestroy(sTransientMem);
    dkMemBlockDestroy(sCmdMem);
    dkMemBlockDestroy(sStaticMem);
    dkSwapchainDestroy(sSwapchain);
    dkMemBlockDestroy(sImageMem);
    dkDeviceDestroy(sDevice);
    sDevice = NULL;
}

struct GfxRenderingAPI gfx_deko3d_api = {
    gfx_deko3d_z_is_from_0_to_1,
    gfx_deko3d_unload_shader,
    gfx_deko3d_load_shader,
    gfx_deko3d_create_and_load_new_shader,
    gfx_deko3d_lookup_shader,
    gfx_deko3d_shader_get_info,
    gfx_deko3d_new_texture,
    gfx_deko3d_select_texture,
    gfx_deko3d_upload_texture,
    gfx_deko3d_set_sampler_parameters,
    gfx_deko3d_set_depth_test,
    gfx_deko3d_set_depth_mask,
    gfx_deko3d_set_zmode_decal,
    gfx_deko3d_set_viewport,
    gfx_deko3d_set_scissor,
    gfx_deko3d_set_use_alpha,
    gfx_deko3d_draw_triangles,
    gfx_deko3d_init,
    gfx_deko3d_on_resize,
    gfx_deko3d_start_frame,
    gfx_deko3d_end_frame,
    gfx_deko3d_finish_render,
    gfx_deko3d_get_name,
    gfx_deko3d_shutdown
};
