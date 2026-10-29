// Nintendo Switch window manager: native window, applet messages (HOME, exit,
// focus, docked/handheld) and timing. Presentation is done by the deko3d renderer.
// Note: libnx headers only, no game headers (u64 clash, see platform_switch.c).

#include <stdio.h>
#include <string.h>

#include <switch.h>

#include "gfx_switch.h"

// game functions, declared here since game headers can't be included
void controller_bind_init(void);
void game_exit(void);

static uint32_t sWidth = SWITCH_HANDHELD_WIDTH;
static uint32_t sHeight = SWITCH_HANDHELD_HEIGHT;
static AppletOperationMode sOperationMode = AppletOperationMode_Handheld;

static kb_callback_t sKbKeyDown = NULL;
static kb_callback_t sKbKeyUp = NULL;
static void (*sKbAllKeysUp)(void) = NULL;
static void (*sKbTextInput)(char*) = NULL;
static void (*sKbTextEditing)(char*, int) = NULL;
static void (*sOnScroll)(float, float) = NULL;

static void gfx_switch_wm_update_output_size(void) {
    sOperationMode = appletGetOperationMode();
    if (sOperationMode == AppletOperationMode_Console) {
        sWidth = SWITCH_DOCKED_WIDTH;
        sHeight = SWITCH_DOCKED_HEIGHT;
    } else {
        sWidth = SWITCH_HANDHELD_WIDTH;
        sHeight = SWITCH_HANDHELD_HEIGHT;
    }
    gfx_deko3d_set_output_size(sWidth, sHeight);
    printf("[switch] %s mode, rendering at %ux%u\n",
           sOperationMode == AppletOperationMode_Console ? "docked" : "handheld", sWidth, sHeight);
}

static void gfx_switch_wm_init(const char *window_title) {
    (void) window_title;
    gfx_deko3d_set_native_window(nwindowGetDefault());
    gfx_switch_wm_update_output_size();
    controller_bind_init();
}

static void gfx_switch_wm_set_keyboard_callbacks(kb_callback_t on_key_down, kb_callback_t on_key_up, void (*on_all_keys_up)(void),
                                                 void (*on_text_input)(char*), void (*on_text_editing)(char*, int)) {
    sKbKeyDown = on_key_down;
    sKbKeyUp = on_key_up;
    sKbAllKeysUp = on_all_keys_up;
    sKbTextInput = on_text_input;
    sKbTextEditing = on_text_editing;
}

static void gfx_switch_wm_set_scroll_callback(void (*on_scroll)(float, float)) {
    sOnScroll = on_scroll;
}

static void gfx_switch_wm_main_loop(void (*run_one_game_iter)(void)) {
    run_one_game_iter();
}

static void gfx_switch_wm_get_dimensions(uint32_t *width, uint32_t *height) {
    if (width) { *width = sWidth; }
    if (height) { *height = sHeight; }
}

static void gfx_switch_wm_handle_events(void) {
    // processes applet messages; blocks while suspended in the HOME menu
    if (!appletMainLoop()) {
        game_exit();
    }

    if (appletGetOperationMode() != sOperationMode) {
        gfx_switch_wm_update_output_size();
    }
}

static bool gfx_switch_wm_start_frame(void) {
    return true;
}

static void gfx_switch_wm_swap_buffers_begin(void) {
    gfx_deko3d_present();
}

static void gfx_switch_wm_swap_buffers_end(void) {
}

static double gfx_switch_wm_get_time(void) {
    return (double) armTicksToNs(armGetSystemTick()) / 1e9;
}

static void gfx_switch_wm_shutdown(void) {
}

// TODO: software keyboard (Phase 4)
static void gfx_switch_wm_start_text_input(void) {
}

static void gfx_switch_wm_stop_text_input(void) {
}

static char *gfx_switch_wm_get_clipboard_text(void) {
    static char clipboard[WAPI_CLIPBOARD_BUFSIZ] = "";
    return clipboard;
}

static void gfx_switch_wm_set_clipboard_text(const char *text) {
    (void) text;
}

static void gfx_switch_wm_set_cursor_visible(bool visible) {
    (void) visible;
}

static void gfx_switch_wm_delay(unsigned int ms) {
    svcSleepThread((s64) ms * 1000000);
}

static int gfx_switch_wm_get_max_msaa(void) {
    return 0;
}

static void gfx_switch_wm_set_window_title(const char *title) {
    (void) title;
}

static void gfx_switch_wm_reset_window_title(void) {
}

static bool gfx_switch_wm_has_focus(void) {
    return appletGetFocusState() == AppletFocusState_InFocus;
}

struct GfxWindowManagerAPI gfx_switch_wm = {
    gfx_switch_wm_init,
    gfx_switch_wm_set_keyboard_callbacks,
    gfx_switch_wm_set_scroll_callback,
    gfx_switch_wm_main_loop,
    gfx_switch_wm_get_dimensions,
    gfx_switch_wm_handle_events,
    gfx_switch_wm_start_frame,
    gfx_switch_wm_swap_buffers_begin,
    gfx_switch_wm_swap_buffers_end,
    gfx_switch_wm_get_time,
    gfx_switch_wm_shutdown,
    gfx_switch_wm_start_text_input,
    gfx_switch_wm_stop_text_input,
    gfx_switch_wm_get_clipboard_text,
    gfx_switch_wm_set_clipboard_text,
    gfx_switch_wm_set_cursor_visible,
    gfx_switch_wm_delay,
    gfx_switch_wm_get_max_msaa,
    gfx_switch_wm_set_window_title,
    gfx_switch_wm_reset_window_title,
    gfx_switch_wm_has_focus
};
