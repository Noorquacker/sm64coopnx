// deko3d rendering backend for Nintendo Switch.
//
// The device uses -1..1 depth like OpenGL, so Fast3D's depth values pass through
// unchanged. The origin is upper-left (clip y = +1 is the top of the screen, memory
// row 0 is the top row as scanned out), so Fast3D's bottom-up viewport and scissor
// rectangles are flipped against the output height. DkDeviceFlags_OriginLowerLeft
// would avoid the flip but renders upside down: like OpenGL textures, row 0 becomes
// the bottom, while the display shows row 0 at the top.
//
// Phase 2: device, swapchain, per-frame command/vertex memory and a test triangle.
// Fast3D drawing (shaders, textures, state) is Phase 3; until then the Fast3D
// entry points are stubs, like gfx_dummy.c.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include <deko3d.h>

#include "macros.h"
#include "pc/configfile.h"
#include "pc/platform.h"
#include "pc/gfx/gfx_cc.h"
#include "gfx_switch.h"

#define FB_NUM 3
#define FB_WIDTH  SWITCH_DOCKED_WIDTH
#define FB_HEIGHT SWITCH_DOCKED_HEIGHT

#define FRAMES_IN_FLIGHT 2
#define CODE_MEM_SIZE      (4 * 1024 * 1024)
#define CMD_MEM_PER_FRAME  (1 * 1024 * 1024)
#define VTX_MEM_PER_FRAME  (8 * 1024 * 1024)

#define ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((a) - 1))

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

// shader code, bump allocated
static DkMemBlock sCodeMem;
static uint32_t sCodeMemOffset = 0;

// command and vertex memory, one slice per frame in flight
static DkMemBlock sCmdMem;
static DkCmdBuf sCmdBuf;
static DkMemBlock sVtxMem;
static uint32_t sVtxOffset = 0;
static DkFence sFrameFences[FRAMES_IN_FLIGHT];
static uint32_t sFrameSlot = 0;

static uint32_t sSwapInterval = UINT32_MAX;
static int sImageSlot = -1;
static bool sFrameActive = false;
static uint32_t sPresentCount = 0;

// framebuffer capture for debugging, see gfx_deko3d_request_capture()
static DkMemBlock sCaptureMem = NULL;
static char sCapturePath[256];
static volatile bool sCaptureRequested = false;

// Phase 2 test triangle
static DkShader sTestVertexShader;
static DkShader sTestFragmentShader;
static bool sTestShadersLoaded = false;

  /////////////////////
 // memory & setup //
/////////////////////

static void gfx_deko3d_debug_callback(UNUSED void *userData, const char *context, DkResult result, const char *message) {
    fprintf(stderr, "[deko3d] %s: %s (result %d)\n", context, message, (int) result);
    if (result != DkResult_Success) {
        sys_fatal("deko3d error in %s:\n%s", context, message);
    }
}

static void gfx_deko3d_cmdbuf_out_of_memory(UNUSED void *userData, UNUSED DkCmdBuf cmdbuf, size_t minReqSize) {
    sys_fatal("deko3d: frame command memory exhausted (%u bytes per frame, %u more needed)",
              CMD_MEM_PER_FRAME, (unsigned) minReqSize);
}

static DkMemBlock gfx_deko3d_memblock_create(uint32_t size, uint32_t flags) {
    DkMemBlockMaker maker;
    dkMemBlockMakerDefaults(&maker, sDevice, ALIGN_UP(size, DK_MEMBLOCK_ALIGNMENT));
    maker.flags = flags;
    return dkMemBlockCreate(&maker);
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

static bool gfx_deko3d_load_shader_file(DkShader *shader, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[deko3d] could not open shader %s\n", path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    uint32_t size = ftell(f);
    rewind(f);

    if (sCodeMemOffset + size > CODE_MEM_SIZE - DK_SHADER_CODE_UNUSABLE_SIZE) {
        fclose(f);
        sys_fatal("deko3d: out of shader code memory");
    }

    uint32_t codeOffset = sCodeMemOffset;
    sCodeMemOffset = ALIGN_UP(sCodeMemOffset + size, DK_SHADER_CODE_ALIGNMENT);
    bool ok = fread((uint8_t *) dkMemBlockGetCpuAddr(sCodeMem) + codeOffset, size, 1, f) == 1;
    fclose(f);
    if (!ok) { return false; }

    DkShaderMaker shaderMaker;
    dkShaderMakerDefaults(&shaderMaker, sCodeMem, codeOffset);
    dkShaderInitialize(shader, &shaderMaker);
    return true;
}

// copies data into this frame's vertex memory, returns its GPU address
static DkGpuAddr gfx_deko3d_push_vertices(const void *data, uint32_t size) {
    uint32_t offset = ALIGN_UP(sVtxOffset, 16);
    if (offset + size > VTX_MEM_PER_FRAME) {
        sys_fatal("deko3d: frame vertex memory exhausted (%u bytes per frame)", VTX_MEM_PER_FRAME);
    }
    sVtxOffset = offset + size;

    uint32_t blockOffset = sFrameSlot * VTX_MEM_PER_FRAME + offset;
    memcpy((uint8_t *) dkMemBlockGetCpuAddr(sVtxMem) + blockOffset, data, size);
    return dkMemBlockGetGpuAddr(sVtxMem) + blockOffset;
}

  /////////////////////
 // test triangle //
/////////////////////

// a spinning triangle drawn over the frame, proves the whole pipeline works end to end
static void gfx_deko3d_draw_test_triangle(void) {
    if (!sTestShadersLoaded) { return; }

    struct TestVertex { float pos[2]; float color[4]; };
    static const DkVtxAttribState attribs[] = {
        { .bufferId = 0, .isFixed = 0, .offset = 0, .size = DkVtxAttribSize_2x32, .type = DkVtxAttribType_Float, .isBgra = 0 },
        { .bufferId = 0, .isFixed = 0, .offset = 8, .size = DkVtxAttribSize_4x32, .type = DkVtxAttribType_Float, .isBgra = 0 },
    };
    static const DkVtxBufferState buffers[] = { { sizeof(struct TestVertex), 0 } };

    float angle = (float) sPresentCount * (2.0f * 3.14159265f / 180.0f); // one turn every 3 s at 60 fps
    float aspect = (float) sOutputHeight / (float) sOutputWidth;
    struct TestVertex verts[3];
    for (int i = 0; i < 3; i++) {
        float a = angle + (float) i * (2.0f * 3.14159265f / 3.0f);
        verts[i].pos[0] = 0.5f * __builtin_sinf(a) * aspect;
        verts[i].pos[1] = 0.5f * __builtin_cosf(a);
        verts[i].color[0] = i == 0;
        verts[i].color[1] = i == 1;
        verts[i].color[2] = i == 2;
        verts[i].color[3] = 1.0f;
    }

    DkShader const *shaders[] = { &sTestVertexShader, &sTestFragmentShader };
    DkRasterizerState rasterizerState;
    DkColorState colorState;
    DkColorWriteState colorWriteState;
    DkDepthStencilState depthState;
    dkRasterizerStateDefaults(&rasterizerState);
    rasterizerState.cullMode = DkFace_None;
    dkColorStateDefaults(&colorState);
    dkColorWriteStateDefaults(&colorWriteState);
    dkDepthStencilStateDefaults(&depthState);
    depthState.depthTestEnable = false;
    depthState.depthWriteEnable = false;

    dkCmdBufBindShaders(sCmdBuf, DkStageFlag_GraphicsMask, shaders, 2);
    dkCmdBufBindRasterizerState(sCmdBuf, &rasterizerState);
    dkCmdBufBindColorState(sCmdBuf, &colorState);
    dkCmdBufBindColorWriteState(sCmdBuf, &colorWriteState);
    dkCmdBufBindDepthStencilState(sCmdBuf, &depthState);
    dkCmdBufBindVtxAttribState(sCmdBuf, attribs, 2);
    dkCmdBufBindVtxBufferState(sCmdBuf, buffers, 1);
    dkCmdBufBindVtxBuffer(sCmdBuf, 0, gfx_deko3d_push_vertices(verts, sizeof(verts)), sizeof(verts));
    dkCmdBufDraw(sCmdBuf, DkPrimitive_Triangles, 3, 1, 0, 0);
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

// TODO Phase 3: color combiner shaders (runtime GLSL -> uam -> DKSH)
static void gfx_deko3d_unload_shader(UNUSED struct ShaderProgram *old_prg) {
}

static void gfx_deko3d_load_shader(UNUSED struct ShaderProgram *new_prg) {
}

static struct ShaderProgram *gfx_deko3d_create_and_load_new_shader(UNUSED struct ColorCombiner *cc) {
    return NULL;
}

static struct ShaderProgram *gfx_deko3d_lookup_shader(UNUSED struct ColorCombiner *cc) {
    return NULL;
}

static void gfx_deko3d_shader_get_info(UNUSED struct ShaderProgram *prg, uint8_t *num_inputs, bool used_textures[2]) {
    *num_inputs = 0;
    used_textures[0] = false;
    used_textures[1] = false;
}

// TODO Phase 3: textures and samplers
static uint32_t gfx_deko3d_new_texture(void) {
    return 0;
}

static void gfx_deko3d_select_texture(UNUSED int tile, UNUSED uint32_t texture_id) {
}

static void gfx_deko3d_upload_texture(UNUSED const uint8_t *rgba32_buf, UNUSED int width, UNUSED int height) {
}

static void gfx_deko3d_set_sampler_parameters(UNUSED int tile, UNUSED bool linear_filter, UNUSED uint32_t cms, UNUSED uint32_t cmt) {
}

// TODO Phase 3: render state. Viewport/scissor y must be flipped: y = outputHeight - y - height
static void gfx_deko3d_set_depth_test(UNUSED bool depth_test) {
}

static void gfx_deko3d_set_depth_mask(UNUSED bool z_upd) {
}

static void gfx_deko3d_set_zmode_decal(UNUSED bool zmode_decal) {
}

static void gfx_deko3d_set_viewport(UNUSED int x, UNUSED int y, UNUSED int width, UNUSED int height) {
}

static void gfx_deko3d_set_scissor(UNUSED int x, UNUSED int y, UNUSED int width, UNUSED int height) {
}

static void gfx_deko3d_set_use_alpha(UNUSED bool use_alpha) {
}

static void gfx_deko3d_draw_triangles(UNUSED float buf_vbo[], UNUSED size_t buf_vbo_len, UNUSED size_t buf_vbo_num_tris) {
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

    sCodeMem = gfx_deko3d_memblock_create(CODE_MEM_SIZE, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code);
    sCmdMem = gfx_deko3d_memblock_create(FRAMES_IN_FLIGHT * CMD_MEM_PER_FRAME, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
    sVtxMem = gfx_deko3d_memblock_create(FRAMES_IN_FLIGHT * VTX_MEM_PER_FRAME, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);

    DkCmdBufMaker cmdBufMaker;
    dkCmdBufMakerDefaults(&cmdBufMaker, sDevice);
    cmdBufMaker.cbAddMem = gfx_deko3d_cmdbuf_out_of_memory;
    sCmdBuf = dkCmdBufCreate(&cmdBufMaker);

    DkQueueMaker queueMaker;
    dkQueueMakerDefaults(&queueMaker, sDevice);
    queueMaker.flags = DkQueueFlags_Graphics;
    sQueue = dkQueueCreate(&queueMaker);

    sTestShadersLoaded = gfx_deko3d_load_shader_file(&sTestVertexShader, SWITCH_SHADER_DIR "/test_triangle_vsh.dksh")
                      && gfx_deko3d_load_shader_file(&sTestFragmentShader, SWITCH_SHADER_DIR "/test_color_fsh.dksh");

    printf("[deko3d] initialized: %d framebuffers of %dx%d, %d frames in flight\n", FB_NUM, FB_WIDTH, FB_HEIGHT, FRAMES_IN_FLIGHT);
}

static void gfx_deko3d_on_resize(void) {
}

static void gfx_deko3d_start_frame(void) {
    // wait until the GPU is done with this slot's command and vertex memory
    dkFenceWait(&sFrameFences[sFrameSlot], -1);

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

    dkCmdBufClear(sCmdBuf);
    dkCmdBufAddMemory(sCmdBuf, sCmdMem, sFrameSlot * CMD_MEM_PER_FRAME, CMD_MEM_PER_FRAME);
    sVtxOffset = 0;

    DkImageView colorView;
    DkImageView depthView;
    dkImageViewDefaults(&colorView, &sFramebuffers[sImageSlot]);
    dkImageViewDefaults(&depthView, &sDepthBuffer);
    dkCmdBufBindRenderTarget(sCmdBuf, &colorView, &depthView);

    DkViewport viewport = { 0.0f, 0.0f, (float) sOutputWidth, (float) sOutputHeight, 0.0f, 1.0f };
    DkScissor scissor = { 0, 0, sOutputWidth, sOutputHeight };
    dkCmdBufSetViewports(sCmdBuf, 0, &viewport, 1);
    dkCmdBufSetScissors(sCmdBuf, 0, &scissor, 1);
    dkCmdBufClearColorFloat(sCmdBuf, 0, DkColorMask_RGBA, 0.0f, 0.0f, 0.0f, 1.0f);
    dkCmdBufClearDepthStencil(sCmdBuf, true, 1.0f, 0xFF, 0);

    sFrameActive = true;
}

static void gfx_deko3d_end_frame(void) {
    if (!sFrameActive) { return; }

    gfx_deko3d_draw_test_triangle();

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

static void gfx_deko3d_shutdown(void) {
    if (!sDevice) { return; }
    dkQueueWaitIdle(sQueue);
    dkQueueDestroy(sQueue);
    dkCmdBufDestroy(sCmdBuf);
    dkMemBlockDestroy(sVtxMem);
    dkMemBlockDestroy(sCmdMem);
    dkMemBlockDestroy(sCodeMem);
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
