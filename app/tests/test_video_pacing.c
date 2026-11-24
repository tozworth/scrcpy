#include <assert.h>

#include "video_pacing.h"

static void
test_jitter(void) {
    struct sc_video_pacing pacing = {0};
    sc_tick delay = SC_TICK_FROM_MS(6);
    assert(!sc_video_pacing_deadline(&pacing, 0, 10000, delay));

    // The 9 ms spike exceeds the buffer and should be released on arrival.
    assert(sc_video_pacing_deadline(&pacing, 16000, 35000, delay) == 32000);
    assert(sc_video_pacing_deadline(&pacing, 32000, 42000, delay) == 48000);

    // An idle update starts a new cadence even if its receive offset changed.
    assert(!sc_video_pacing_deadline(&pacing, 80000, 100000, delay));
    assert(sc_video_pacing_deadline(&pacing, 96000, 116000, delay) == 122000);
}

static void
test_sustained_offset_change(void) {
    struct sc_video_pacing pacing = {0};
    sc_tick delay = SC_TICK_FROM_MS(6);
    assert(!sc_video_pacing_deadline(&pacing, 0, 10000, delay));

    // Once the old minimum ages out, the buffer tracks the new offset.
    for (unsigned i = 1; i < SC_VIDEO_PACING_WINDOW; ++i) {
        sc_tick pts = i * SC_TICK_FROM_MS(16);
        assert(sc_video_pacing_deadline(&pacing, pts, pts + 20000, delay)
               == pts + 16000);
    }
    sc_tick pts = SC_VIDEO_PACING_WINDOW * SC_TICK_FROM_MS(16);
    assert(sc_video_pacing_deadline(&pacing, pts, pts + 20000, delay)
           == pts + 26000);

    // A source clock reset starts a new cadence.
    assert(!sc_video_pacing_deadline(&pacing, 16000, pts + 30000, delay));
}

int
main(void) {
    test_jitter();
    test_sustained_offset_change();
    return 0;
}
