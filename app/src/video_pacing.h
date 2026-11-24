#ifndef SC_VIDEO_PACING_H
#define SC_VIDEO_PACING_H

#include <stdbool.h>

#include "util/tick.h"

#define SC_VIDEO_PACING_WINDOW 24

struct sc_video_pacing {
    sc_tick offsets[SC_VIDEO_PACING_WINDOW];
    sc_tick previous_pts;
    unsigned count;
    unsigned next;
};

// Return 0 to release the first frame of an update immediately. During
// animation, use the fastest recent arrival to anchor the stream clock.
// A slow arrival consumes its own jitter budget instead of raising it for
// subsequent frames. The window lets the clock follow lasting changes.
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

    for (unsigned i = 0; i < pacing->count; ++i) {
        if (pacing->offsets[i] < offset) {
            offset = pacing->offsets[i];
        }
    }
    return pts + offset + delay;
}

#endif
