/* appcpy modification: keyboard-to-touch mapping tests. */
#include <assert.h>
#include <string.h>
#include "keymap.h"

static unsigned
parse(struct sc_keymap *keymap, const char *text) {
    return sc_keymap_parse(keymap, text, strlen(text));
}

static void test_parse(void) {
    struct sc_keymap km;
    const char *text =
        "# appcpy keymap 1\r\n"
        "\n"
        "key 26 0 100 120 -80 W\r\n"
        "  key\t44 100 100 -96 -64 Space bar\n"
        "swipe 1 2 3 4 5\n"             // unknown kinds are ignored
        "key 26 50 50 0 0 duplicate\n"  // skipped
        "key 0 0 0 0 0\n"               // invalid scancode
        "key 512 0 0 0 0\n"             // invalid scancode
        "key 4 101 0 0 0\n"             // invalid anchor
        "key 4 0 0 20001 0\n"           // offset too large
        "key 4 0 0 1.5 0\n"             // not an integer
        "key 4 0 0 0\n"                 // missing field
        "keys 4 0 0 0 0\n"              // other kind
        "key 80 50 50 -10 +0";          // no trailing newline
    assert(parse(&km, text) == 7);
    assert(km.count == 3);
    assert(km.bindings[0].scancode == 26);
    assert(!km.bindings[0].drag);
    assert(km.bindings[0].start.anchor_x == 0);
    assert(km.bindings[0].start.anchor_y == 100);
    assert(km.bindings[0].start.offset_x == 120);
    assert(km.bindings[0].start.offset_y == -80);
    assert(km.bindings[1].scancode == 44);
    assert(km.bindings[1].start.offset_x == -96);
    assert(km.bindings[2].scancode == 80);
    assert(km.bindings[2].start.offset_x == -10);
    assert(km.bindings[2].start.offset_y == 0);
    assert(sc_keymap_find(&km, 44) == 1);
    assert(sc_keymap_find(&km, 4) == -1);

    assert(parse(&km, "") == 0);
    assert(km.count == 0);
}

static void test_drag(void) {
    struct sc_keymap km;
    assert(parse(&km, "drag\t44 0 100 120 -80 100 50 -60 10 Space bar\r\n"
                      "key 44 0 0 0 0 duplicate\n"
                      "drag 26 0 0 0 0 100 0 -60\n"
                      "drag 26 0 0 0 0 101 0 -60 0\n"
                      "key 26 0 0 0 0") == 3);
    assert(km.count == 2);
    assert(km.bindings[0].drag && km.bindings[0].scancode == 44);
    assert(!km.bindings[1].drag && km.bindings[1].scancode == 26);
    int32_t x, y;
    assert(sc_keymap_get_point(&km.bindings[0].start, 1280, 800, 240, &x, &y));
    assert(x == 180 && y == 680);
    assert(sc_keymap_get_point(&km.bindings[0].end, 1280, 800, 240, &x, &y));
    assert(x == 1190 && y == 415);
}

static void test_limit(void) {
    struct sc_keymap km;
    char text[8192] = "";
    for (int i = 1; i <= SC_KEYMAP_MAX_BINDINGS + 2; ++i) {
        char line[64];
        int n = 0;
        // Build "key <i> 0 0 0 0\n" without stdio.
        memcpy(line, "key ", 4);
        n = 4;
        if (i >= 10) {
            line[n++] = '0' + i / 10;
        }
        line[n++] = '0' + i % 10;
        memcpy(line + n, " 0 0 0 0\n", 10);
        strcat(text, line);
    }
    assert(parse(&km, text) == 2);
    assert(km.count == SC_KEYMAP_MAX_BINDINGS);
}

static void test_point(void) {
    struct sc_keymap_point b = {
        .anchor_x = 0, .anchor_y = 100,
        .offset_x = 100, .offset_y = -50,
    };
    int32_t x, y;
    // 100 dp at 240 dpi is 150 px from the left; -50 dp is 75 px up.
    assert(sc_keymap_get_point(&b, 1280, 800, 240, &x, &y));
    assert(x == 150 && y == 725);
    // Resizing keeps the distance from the anchor.
    assert(sc_keymap_get_point(&b, 1920, 1080, 240, &x, &y));
    assert(x == 150 && y == 1005);
    // At half resolution and half density, the point scales with the content.
    assert(sc_keymap_get_point(&b, 640, 400, 120, &x, &y));
    assert(x == 75 && y == 362);

    struct sc_keymap_point c = {
        .anchor_x = 50, .anchor_y = 50,
        .offset_x = -20, .offset_y = 20,
    };
    assert(sc_keymap_get_point(&c, 1000, 600, 160, &x, &y));
    assert(x == 480 && y == 320);

    // Points outside the content are clamped inside it.
    struct sc_keymap_point far = {
        .anchor_x = 100, .anchor_y = 0,
        .offset_x = 5000, .offset_y = -5000,
    };
    assert(sc_keymap_get_point(&far, 1000, 600, 160, &x, &y));
    assert(x == 999 && y == 0);
    assert(!sc_keymap_get_point(&far, 0, 600, 160, &x, &y));
}

static void test_dpi(void) {
    assert(sc_keymap_parse_display_dpi("1280x800/240") == 240);
    assert(sc_keymap_parse_display_dpi("/120") == 120);
    assert(sc_keymap_parse_display_dpi("1280x800") == 0);
    assert(sc_keymap_parse_display_dpi("1280x800/") == 0);
    assert(sc_keymap_parse_display_dpi("1280x800/24a") == 0);
    assert(sc_keymap_parse_display_dpi("/70000") == 0);
    assert(sc_keymap_parse_display_dpi(NULL) == 0);
}

int main(void) {
    test_parse();
    test_drag();
    test_limit();
    test_point();
    test_dpi();
    return 0;
}
