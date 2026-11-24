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
    // Keep pacing regular 30 fps frames, including some timestamp jitter.
    bool continuous = pacing->count && pts > pacing->previous_pts
                   && pts - pacing->previous_pts <= SC_TICK_FROM_MS(40);
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
    // Aim to cover the usual late arrivals and let isolated spikes through.
    // Stable streams only need the time implied by their observed jitter.
    sc_tick late_offset = sorted[count - 1 - (count - 1) / 20];
    sc_tick early_offset = sorted[(count - 1) / 5];
    // Small headroom for variation beyond the recent arrivals. A stream
    // without jitter does not need any headroom.
    sc_tick spread = late_offset - early_offset;
    sc_tick headroom = SC_TICK_FROM_US(500);
    sc_tick target_offset = late_offset + (spread < headroom ? spread
                                                           : headroom);
    sc_tick capped_offset = (early_offset < offset ? early_offset : offset)
                          + delay;
    return pts + (target_offset < capped_offset ? target_offset
                                                : capped_offset);
}

#endif
