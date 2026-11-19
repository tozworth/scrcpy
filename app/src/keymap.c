/* appcpy modification: keyboard-to-touch mapping. */
#include "keymap.h"

#include <limits.h>
#include <string.h>

static bool
is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r';
}

static const char *
skip_spaces(const char *p, const char *end) {
    while (p < end && is_space(*p)) {
        ++p;
    }
    return p;
}

// Parse a decimal integer without locale or errno dependencies.
static bool
parse_int(const char **pp, const char *end, long min, long max, long *out) {
    const char *p = skip_spaces(*pp, end);
    bool negative = false;
    if (p < end && (*p == '-' || *p == '+')) {
        negative = *p == '-';
        ++p;
    }
    if (p == end || *p < '0' || *p > '9') {
        return false;
    }
    long value = 0;
    while (p < end && *p >= '0' && *p <= '9') {
        value = value * 10 + (*p - '0');
        if (value > 1000000) {
            return false;
        }
        ++p;
    }
    if (p < end && !is_space(*p)) {
        return false;
    }
    if (negative) {
        value = -value;
    }
    if (value < min || value > max) {
        return false;
    }
    *out = value;
    *pp = p;
    return true;
}

int
sc_keymap_find(const struct sc_keymap *keymap, uint16_t scancode) {
    if (!scancode) {
        return -1;
    }
    for (unsigned i = 0; i < keymap->count; ++i) {
        const struct sc_keymap_binding *binding = &keymap->bindings[i];
        if (binding->scancode == scancode) {
            return (int) i;
        }
        if (binding->range_dp) {
            for (unsigned d = 0; d < 4; ++d) {
                if (binding->joystick_keys[d] == scancode) {
                    return (int) i;
                }
            }
        }
    }
    return -1;
}

static bool
parse_point(const char **p, const char *end, struct sc_keymap_point *point) {
    long ax, ay, dx, dy;
    if (!parse_int(p, end, 0, 100, &ax)
            || !parse_int(p, end, 0, 100, &ay)
            || !parse_int(p, end, -SC_KEYMAP_MAX_OFFSET_DP,
                          SC_KEYMAP_MAX_OFFSET_DP, &dx)
            || !parse_int(p, end, -SC_KEYMAP_MAX_OFFSET_DP,
                          SC_KEYMAP_MAX_OFFSET_DP, &dy)) {
        return false;
    }
    point->anchor_x = (uint8_t) ax;
    point->anchor_y = (uint8_t) ay;
    point->offset_x = (int32_t) dx;
    point->offset_y = (int32_t) dy;
    return true;
}

static bool
parse_joystick(const struct sc_keymap *keymap, const char *p, const char *end,
               struct sc_keymap_binding *binding) {
    long deadzone, range;
    if (!parse_point(&p, end, &binding->start)
            || !parse_int(&p, end, 0, SC_KEYMAP_MAX_OFFSET_DP, &deadzone)
            || !parse_int(&p, end, deadzone + 1, SC_KEYMAP_MAX_OFFSET_DP,
                          &range)) {
        return false;
    }
    for (unsigned d = 0; d < 4; ++d) {
        long key;
        if (!parse_int(&p, end, 0, SC_KEYMAP_MAX_SCANCODE, &key)
                || sc_keymap_find(keymap, (uint16_t) key) >= 0) {
            return false;
        }
        for (unsigned other = 0; key && other < d; ++other) {
            if (binding->joystick_keys[other] == key) {
                return false;
            }
        }
        binding->joystick_keys[d] = (uint16_t) key;
    }
    binding->deadzone_dp = (uint16_t) deadzone;
    binding->range_dp = (uint16_t) range;
    return skip_spaces(p, end) == end;
}

unsigned
sc_keymap_parse(struct sc_keymap *keymap, const char *text, size_t len) {
    keymap->count = 0;
    unsigned skipped = 0;
    uint8_t drag_groups[SC_KEYMAP_MAX_SCANCODE + 1] = {0};
    const char *end = text + len;
    const char *line = text;
    while (line < end) {
        const char *eol = memchr(line, '\n', end - line);
        if (!eol) {
            eol = end;
        }
        const char *p = skip_spaces(line, eol);
        line = eol == end ? end : eol + 1;
        if (p == eol || *p == '#') {
            continue;
        }
        if (eol - p >= 9 && !memcmp(p, "joystick", 8) && is_space(p[8])) {
            struct sc_keymap_binding binding = {0};
            if (keymap->count == SC_KEYMAP_MAX_BINDINGS
                    || !parse_joystick(keymap, p + 8, eol, &binding)) {
                ++skipped;
            } else {
                keymap->bindings[keymap->count++] = binding;
            }
            continue;
        }
        if (eol - p >= 11 && !memcmp(p, "drag-group", 10) && is_space(p[10])) {
            p += 10;
            long scancode, group;
            if (parse_int(&p, eol, 1, SC_KEYMAP_MAX_SCANCODE, &scancode)
                    && parse_int(&p, eol, 1, SC_KEYMAP_MAX_BINDINGS, &group)
                    && skip_spaces(p, eol) == eol) {
                drag_groups[scancode] = (uint8_t) group;
            } else {
                ++skipped;
            }
            continue;
        }
        bool drag = eol - p >= 5 && !memcmp(p, "drag", 4) && is_space(p[4]);
        if (!drag && (eol - p < 4 || memcmp(p, "key", 3) || !is_space(p[3]))) {
            // Unknown line kinds are reserved for later versions.
            continue;
        }
        struct sc_keymap_binding b = {.drag = drag};
        long scancode;
        p += drag ? 4 : 3;
        if (!parse_int(&p, eol, 1, SC_KEYMAP_MAX_SCANCODE, &scancode)
                || !parse_point(&p, eol, &b.start)
                || (drag && !parse_point(&p, eol, &b.end))
                || sc_keymap_find(keymap, (uint16_t) scancode) >= 0
                || keymap->count == SC_KEYMAP_MAX_BINDINGS) {
            ++skipped;
            continue;
        }
        b.scancode = (uint16_t) scancode;
        keymap->bindings[keymap->count++] = b;
    }
    for (unsigned i = 0; i < keymap->count; ++i) {
        struct sc_keymap_binding *binding = &keymap->bindings[i];
        if (binding->drag) {
            binding->drag_group = drag_groups[binding->scancode];
        }
    }
    return skipped;
}

static int32_t
get_coordinate(uint32_t size, uint8_t anchor, int32_t offset, uint16_t dpi) {
    int64_t value = (int64_t) size * anchor / 100
                  + ((int64_t) offset * dpi + (offset < 0 ? -80 : 80)) / 160;
    if (value < 0) {
        return 0;
    }
    if (value >= size) {
        return (int32_t) size - 1;
    }
    return (int32_t) value;
}

bool
sc_keymap_get_point(const struct sc_keymap_point *point,
                    uint32_t content_width, uint32_t content_height,
                    uint16_t dpi, int32_t *x, int32_t *y) {
    if (!content_width || !content_height || content_width > INT32_MAX
            || content_height > INT32_MAX) {
        return false;
    }
    *x = get_coordinate(content_width, point->anchor_x, point->offset_x,
                        dpi);
    *y = get_coordinate(content_height, point->anchor_y, point->offset_y,
                        dpi);
    return true;
}

uint16_t
sc_keymap_parse_display_dpi(const char *new_display) {
    if (!new_display) {
        return 0;
    }
    const char *slash = strchr(new_display, '/');
    if (!slash || !slash[1]) {
        return 0;
    }
    long dpi = 0;
    for (const char *p = slash + 1; *p; ++p) {
        if (*p < '0' || *p > '9') {
            return 0;
        }
        dpi = dpi * 10 + (*p - '0');
        if (dpi > UINT16_MAX) {
            return 0;
        }
    }
    return (uint16_t) dpi;
}
