#ifndef GFX_SWITCH_H
#define GFX_SWITCH_H

// Glue between the Switch window manager (gfx_switch_wm.c, uses libnx) and the
// deko3d renderer (gfx_deko3d.c, uses game headers). libnx and the game define
// u64 differently, so this header only uses standard types.

#include <stdint.h>

#include "pc/gfx/gfx_window_manager_api.h"
#include "pc/gfx/gfx_rendering_api.h"

// handheld renders at 720p, docked at 1080p
#define SWITCH_HANDHELD_WIDTH  1280
#define SWITCH_HANDHELD_HEIGHT 720
#define SWITCH_DOCKED_WIDTH    1920
#define SWITCH_DOCKED_HEIGHT   1080

extern struct GfxWindowManagerAPI gfx_switch_wm;
extern struct GfxRenderingAPI gfx_deko3d_api;

// window manager -> renderer
void gfx_deko3d_set_native_window(void *nwindow); // NWindow*, before init()
void gfx_deko3d_set_output_size(uint32_t width, uint32_t height);
void gfx_deko3d_present(void);

// number of frames presented so far (for logging)
uint32_t gfx_deko3d_get_present_count(void);

// debug: write the next frame's framebuffer to path as a PPM (thread safe)
void gfx_deko3d_request_capture(const char *path);

#endif
