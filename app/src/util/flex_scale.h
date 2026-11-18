/* appcpy modification: percentage scaling for flex displays. */
#ifndef SC_FLEX_SCALE_H
#define SC_FLEX_SCALE_H

#include <stdbool.h>
#include <stdint.h>

static inline unsigned
sc_flex_requested_integer_factor(uint8_t percent) {
    return percent == 50 ? 2 : percent == 33 ? 3 : percent == 25 ? 4 : 0;
}

// Keep tiny windows encodable and use wide intermediates for large displays.
static inline uint16_t
sc_flex_scale_dimension(uint16_t value, uint8_t percent, bool integer_scaling) {
    // 33% snaps to one third only when integer scaling is requested.
    uint32_t scaled = integer_scaling && percent == 33
                    ? value / 3 : (uint32_t) value * percent / 100;
    return scaled < 16 ? 16 : scaled;
}

static inline uint16_t
sc_flex_unscale_dimension(uint16_t value, uint8_t percent, bool integer_scaling) {
    uint32_t scaled = integer_scaling && percent == 33
                    ? (uint32_t) value * 3
                    : ((uint32_t) value * 100 + percent / 2) / percent;
    return scaled > UINT16_MAX ? UINT16_MAX : scaled;
}

// Use only the requested exact factor. Oversized/stale frames during resize
// fall back to normal rendering until the scaled display catches up.
static inline unsigned
sc_flex_integer_factor(uint32_t width, uint32_t height,
                       uint16_t frame_width, uint16_t frame_height,
                       uint8_t percent) {
    unsigned factor = sc_flex_requested_integer_factor(percent);
    if (!factor || !frame_width || !frame_height
            || (uint32_t) frame_width * factor > width
            || (uint32_t) frame_height * factor > height) {
        return 0;
    }
    return factor;
}

#endif
