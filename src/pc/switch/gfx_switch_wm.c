// Nintendo Switch window manager: native window, applet messages (HOME, exit,
// focus, docked/handheld) and timing. Presentation is done by the deko3d renderer.
// Note: libnx headers only, no game headers (u64 clash, see platform_switch.c).

#include <stdio.h>
#include <string.h>

#include <switch.h>
#include <SDL2/SDL.h>

#include "gfx_switch.h"
#include "platform_switch.h"

// devkitPro's SDL maps the Switch buttons by position, Xbox style (the button labeled B is SDL's A).
// coopdx binds by SDL name (N64 A = SDL A, N64 B = SDL B), so map them by label instead:
// A jumps and confirms in menus, B punches and goes back.
#define SWITCH_CONTROLLER_MAPPING \
    "000038f853776974636820436f6e7400,Switch Controller," \
    "a:b0,b:b1,x:b2,y:b3,back:b11,start:b10,leftshoulder:b6,rightshoulder:b7,lefttrigger:b8,righttrigger:b9," \
    "leftstick:b4,rightstick:b5,dpup:b13,dpdown:b15,dpleft:b12,dpright:b14,leftx:a0,lefty:a1,rightx:a2,righty:a3,"

// game functions, declared here since game headers can't be included
void controller_bind_init(void);
void game_exit(void);

// keyboard scancodes from pc/controller/controller_keyboard.h
#define SCANCODE_ESCAPE        1
#define SCANCODE_BACKSPACE    14
#define SCANCODE_ENTER        28
#define SCANCODE_CONTROL_LEFT 29
#define SCANCODE_A            30
#define SCANCODE_C            46

static uint32_t sWidth = SWITCH_HANDHELD_WIDTH;
static uint32_t sHeight = SWITCH_HANDHELD_HEIGHT;
static AppletOperationMode sOperationMode = AppletOperationMode_Handheld;

static kb_callback_t sKbKeyDown = NULL;
static kb_callback_t sKbKeyUp = NULL;
static void (*sKbAllKeysUp)(void) = NULL;
static void (*sKbTextInput)(char*) = NULL;
static void (*sKbTextEditing)(char*, int) = NULL;
static void (*sOnScroll)(float, float) = NULL;

static char sClipboard[WAPI_CLIPBOARD_BUFSIZ] = "";
static bool sTextInputPending = false;

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
    // read when SDL's game controller subsystem starts, which happens later (controller_sdl.c)
    SDL_SetHint(SDL_HINT_GAMECONTROLLERCONFIG, SWITCH_CONTROLLER_MAPPING);
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

static void gfx_switch_wm_press_key(int scancode) {
    if (sKbKeyDown) { sKbKeyDown(scancode); }
    if (sKbKeyUp) { sKbKeyUp(scancode); }
}

static void gfx_switch_wm_press_ctrl_key(int scancode) {
    if (sKbKeyDown) { sKbKeyDown(SCANCODE_CONTROL_LEFT); }
    gfx_switch_wm_press_key(scancode);
    if (sKbKeyUp) { sKbKeyUp(SCANCODE_CONTROL_LEFT); }
}

// A focused DJUI text field gets edited with the system keyboard as a whole. Everything goes through
// the regular keyboard callbacks, from handle_events() like SDL's keyboard events on PC:
// Ctrl+A and Ctrl+C fetch the current text (through set_clipboard_text), the keyboard's result replaces
// it, then Enter confirms (sends a chat message, connects, ...). Cancelling the keyboard presses Esc.
static void gfx_switch_wm_edit_text(void) {
    sClipboard[0] = '\0';
    gfx_switch_wm_press_ctrl_key(SCANCODE_A);
    gfx_switch_wm_press_ctrl_key(SCANCODE_C);
    char initialText[WAPI_CLIPBOARD_BUFSIZ];
    snprintf(initialText, sizeof(initialText), "%s", sClipboard);

    SwkbdConfig kbd;
    if (R_FAILED(swkbdCreate(&kbd, 0))) {
        fprintf(stderr, "[switch] could not open the software keyboard\n");
        return;
    }
    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetReturnButtonFlag(&kbd, 0); // all text fields are single line
    swkbdConfigSetStringLenMax(&kbd, WAPI_CLIPBOARD_BUFSIZ / 4);
    swkbdConfigSetInitialText(&kbd, initialText);
    char text[WAPI_CLIPBOARD_BUFSIZ] = "";
    Result rc = swkbdShow(&kbd, text, sizeof(text));
    swkbdClose(&kbd);

    if (R_FAILED(rc)) {
        gfx_switch_wm_press_key(SCANCODE_ESCAPE);
        return;
    }

    // replace the whole text (the selection from Ctrl+A); empty text is a deletion
    gfx_switch_wm_press_ctrl_key(SCANCODE_A);
    if (text[0] == '\0') {
        gfx_switch_wm_press_key(SCANCODE_BACKSPACE);
    } else if (sKbTextInput) {
        sKbTextInput(text);
    }
    gfx_switch_wm_press_key(SCANCODE_ENTER);
}

static void gfx_switch_wm_handle_events(void) {
    // processes applet messages; blocks while suspended in the HOME menu
    if (!appletMainLoop() || switch_quit_requested()) {
        game_exit();
    }

    if (appletGetOperationMode() != sOperationMode) {
        gfx_switch_wm_update_output_size();
    }

    if (sTextInputPending) {
        sTextInputPending = false;
        gfx_switch_wm_edit_text();
    }

    switch_show_pending_warnings();
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

// called when a DJUI text field gets focus; the keyboard opens on the next handle_events()
static void gfx_switch_wm_start_text_input(void) {
    sTextInputPending = true;
}

static void gfx_switch_wm_stop_text_input(void) {
    sTextInputPending = false;
}

static char *gfx_switch_wm_get_clipboard_text(void) {
    return sClipboard;
}

static void gfx_switch_wm_set_clipboard_text(const char *text) {
    snprintf(sClipboard, sizeof(sClipboard), "%s", text);
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
