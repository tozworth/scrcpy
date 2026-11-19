/* Modified by appcpy: keyboard-to-touch mapping. */
#ifndef SC_INPUTMANAGER_H
#define SC_INPUTMANAGER_H

#include "common.h"

#include <stdbool.h>
#include <stdint.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_keycode.h>

#include "controller.h"
#include "file_pusher.h"
#include "keymap.h"
#include "options.h"
#include "trait/gamepad_processor.h"
#include "trait/key_processor.h"
#include "trait/mouse_processor.h"

struct sc_keymap_drag {
    struct sc_point start;
    struct sc_point end;
    uint64_t started_ms;
    double duration_ms;
};

struct sc_input_manager {
    struct sc_controller *controller;
    struct sc_file_pusher *fp;
    struct sc_screen *screen;

    struct sc_key_processor *kp;
    struct sc_mouse_processor *mp;
    struct sc_gamepad_processor *gp;

    bool camera;

    struct sc_mouse_bindings mouse_bindings;
    bool legacy_paste;
    bool clipboard_autosync;

    uint16_t sdl_shortcut_mods;

    bool vfinger_down;
    bool vfinger_invert_x;
    bool vfinger_invert_y;

    uint8_t mouse_buttons_state; // OR of enum sc_mouse_button values

    // Tracks the number of identical consecutive shortcut key down events.
    // Not to be confused with event->repeat, which counts the number of
    // system-generated repeated key presses.
    unsigned key_repeat;
    SDL_Keycode last_keycode;
    uint16_t last_mod;

    uint64_t next_sequence; // used for request acknowledgements

    bool disconnected;

    // appcpy: keyboard-to-touch mapping
    const char *keymap_path; // NULL if disabled
    uint16_t keymap_dpi;
    bool keymap_enabled;
    bool keymap_file_present;
    bool keymap_swallow_text;
    int64_t keymap_mtime;
    uint64_t keymap_pressed; // bit per binding index
    uint64_t keymap_moving; // drags that have not yet reached the endpoint
    uint64_t keymap_released; // key ups deferred until the drag reaches its endpoint
    uint64_t keymap_next_step_ms;
    uint64_t keymap_joystick_moved; // active joystick touches that sent a MOVE
    struct sc_keymap keymap;
    struct sc_position keymap_positions[SC_KEYMAP_MAX_BINDINGS];
    struct sc_keymap_drag keymap_drags[SC_KEYMAP_MAX_BINDINGS];
    uint8_t keymap_joystick_held[SC_KEYMAP_MAX_BINDINGS];
    char *keymap_title; // window title while mapping is enabled
};

struct sc_input_manager_params {
    struct sc_controller *controller;
    struct sc_file_pusher *fp;
    struct sc_screen *screen;
    struct sc_key_processor *kp;
    struct sc_mouse_processor *mp;
    struct sc_gamepad_processor *gp;
    bool camera;

    struct sc_mouse_bindings mouse_bindings;
    bool legacy_paste;
    bool clipboard_autosync;
    uint8_t shortcut_mods; // OR of enum sc_shortcut_mod values

    const char *keymap; // appcpy: may be NULL
    uint16_t keymap_dpi; // appcpy: 0 if unknown
};

void
sc_input_manager_init(struct sc_input_manager *im,
                      const struct sc_input_manager_params *params);

void
sc_input_manager_handle_event(struct sc_input_manager *im,
                              const SDL_Event *event);

// Advance moving mapped touches on the event thread, then return the wait time
// until the next step in milliseconds, or -1 if there is no movement.
int
sc_input_manager_step_keymap(struct sc_input_manager *im);

// appcpy: free the keymap state
void
sc_input_manager_destroy(struct sc_input_manager *im);

#endif
