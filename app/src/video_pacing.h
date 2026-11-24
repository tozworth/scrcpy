#ifndef SC_VIDEO_PACING_H
#define SC_VIDEO_PACING_H

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "util/tick.h"

#define SC_VIDEO_PACING_WINDOW 24

struct sc_video_pacing {
    sc_tick offsets[SC_VIDEO_PACING_WINDOW];
    sc_tick previous_pts;
    unsigned count;
    unsigned next;
};

static inline int
sc_video_pacing_compare_offsets(const void *a, const void *b) {
    sc_tick x = *(const sc_tick *) a;
    sc_tick y = *(const sc_tick *) b;
    return (x > y) - (x < y);
}

// Return 0 to release the first frame of an update immediately. Estimate the
// stream clock from the fast end of recent arrivals, so neither a slow packet
// nor a single unusually early packet shifts the deadlines for later frames.
static inline sc_tick
sc_video_pacing_deadline(struct sc_video_pacing *pacing, sc_tick pts,
                         sc_tick received, sc_tick delay) {
    bool continuous = pacing->count && pts > pacing->previous_pts
                   && pts - pacing->previous_pts <= SC_TICK_FROM_MS(32);
    pacing->previous_pts = pts;
    if (!continuous) {
        pacing->count = 0;
        pacing->next = 0;
    }

    sc_tick offset = received - pts;
    pacing->offsets[pacing->next] = offset;
    pacing->next = (pacing->next + 1) % SC_VIDEO_PACING_WINDOW;
    if (pacing->count < SC_VIDEO_PACING_WINDOW) {
        ++pacing->count;
    }

    if (!continuous) {
        return 0;
    }

    sc_tick sorted[SC_VIDEO_PACING_WINDOW];
    size_t count = pacing->count;
    memcpy(sorted, pacing->offsets, count * sizeof(*sorted));
    qsort(sorted, count, sizeof(*sorted), sc_video_pacing_compare_offsets);
    // Use the lower fifth of arrivals, capped by this frame's own offset.
    sc_tick early_offset = sorted[(count - 1) / 5];
    return pts + (early_offset < offset ? early_offset : offset) + delay;
}

#endif
