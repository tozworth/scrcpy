/* appcpy modification: keyboard-to-touch mapping. */
#ifndef SC_KEYMAP_H
#define SC_KEYMAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Bindings beyond this limit are ignored. Android accepts at most 10 pointers
// at once; the input manager keeps one free for the mouse.
#define SC_KEYMAP_MAX_BINDINGS 64
#define SC_KEYMAP_MAX_PRESSED 9
#define SC_KEYMAP_MAX_SCANCODE 511
#define SC_KEYMAP_MAX_OFFSET_DP 20000
#define SC_POINTER_ID_KEYMAP_BASE UINT64_C(0x6170706370790000)

// A key press touches a point relative to an anchor of the displayed content.
// The anchor is a percentage of the width/height (0, 50 or 100 for the nearest
// edge or center). The offset is in density-independent pixels, so the point
// keeps its distance from that anchor when a flex display is resized, as
// Android layouts do.
struct sc_keymap_point {
    uint8_t anchor_x;  // percent
    uint8_t anchor_y;  // percent
    int32_t offset_x;  // dp
    int32_t offset_y;  // dp
};

struct sc_keymap_binding {
    uint16_t scancode; // SDL_Scancode
    bool drag;
    uint8_t drag_group; // zero if independent
    struct sc_keymap_point start;
    struct sc_keymap_point end;
};

struct sc_keymap {
    struct sc_keymap_binding bindings[SC_KEYMAP_MAX_BINDINGS];
    unsigned count;
};

// Parse the keymap text format. Each non-empty line that does not start with
// '#' is:
//
//     key <scancode> <anchor-x%> <anchor-y%> <offset-x-dp> <offset-y-dp> [label]
//     drag <scancode> <start point fields> <end point fields> [label]
//     drag-group <scancode> <group ID>
//
// Lines of other kinds are ignored for forward compatibility. Invalid lines,
// duplicate scancodes and bindings beyond the limit are skipped and counted.
// Returns the number of skipped lines.
unsigned
sc_keymap_parse(struct sc_keymap *keymap, const char *text, size_t len);

// Return the index of the binding for this scancode, or -1.
int
sc_keymap_find(const struct sc_keymap *keymap, uint16_t scancode);

// Compute the content-space point (as displayed, before orientation) for a
// binding, clamped inside the content. Returns false if the size is empty.
bool
sc_keymap_get_point(const struct sc_keymap_point *point,
                    uint32_t content_width, uint32_t content_height,
                    uint16_t dpi, int32_t *x, int32_t *y);

// Extract the density from a --new-display value ([<w>x<h>][/<dpi>]).
// Returns 0 if absent or invalid.
uint16_t
sc_keymap_parse_display_dpi(const char *new_display);

#endif
