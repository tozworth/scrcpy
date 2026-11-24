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
    assert(sc_video_pacing_deadline(&pacing, 96000, 116000, delay) == 116000);
}

static void
test_outliers_on_stable_stream(void) {
    struct sc_video_pacing pacing = {0};
    sc_tick delay = SC_TICK_FROM_MS(6);
    assert(!sc_video_pacing_deadline(&pacing, 0, 20000, delay));

    for (unsigned i = 1; i < SC_VIDEO_PACING_WINDOW; ++i) {
        sc_tick pts = i * SC_TICK_FROM_MS(16);
        assert(sc_video_pacing_deadline(&pacing, pts, pts + 20000, delay)
               == pts + 20000);
    }

    sc_tick pts = SC_VIDEO_PACING_WINDOW * SC_TICK_FROM_MS(16);
    // An early frame keeps its own 6 ms limit without moving the cadence.
    assert(sc_video_pacing_deadline(&pacing, pts, pts + 10000, delay)
           == pts + 16000);
    pts += SC_TICK_FROM_MS(16);
    // A late frame is due immediately and does not delay the following frame.
    assert(sc_video_pacing_deadline(&pacing, pts, pts + 31000, delay)
           == pts + 20000);
    pts += SC_TICK_FROM_MS(16);
    assert(sc_video_pacing_deadline(&pacing, pts, pts + 20000, delay)
           == pts + 20000);
}

static void
test_30_fps_cadence(void) {
    struct sc_video_pacing pacing = {0};
    sc_tick delay = SC_TICK_FROM_MS(6);
    assert(!sc_video_pacing_deadline(&pacing, 0, 20000, delay));
    assert(sc_video_pacing_deadline(&pacing, 33333, 55333, delay) == 55833);
    assert(sc_video_pacing_deadline(&pacing, 66666, 86666, delay) == 89166);
    assert(!sc_video_pacing_deadline(&pacing, 110000, 130000, delay));
}

static void
test_sustained_offset_change(void) {
    struct sc_video_pacing pacing = {0};
    sc_tick delay = SC_TICK_FROM_MS(6);
    assert(!sc_video_pacing_deadline(&pacing, 0, 10000, delay));

    sc_tick period = SC_TICK_FROM_MS(16);
    sc_tick pts = period;
    for (unsigned i = 1; i < SC_VIDEO_PACING_WINDOW; ++i, pts += period) {
        assert(sc_video_pacing_deadline(&pacing, pts, pts + 10000, delay)
               == pts + 10000);
    }

    // Keep the previous cadence until too few of its samples remain.
    unsigned early_samples = (SC_VIDEO_PACING_WINDOW - 1) / 5 + 1;
    unsigned ignored_late_samples = (SC_VIDEO_PACING_WINDOW - 1) / 20;
    for (unsigned i = 0; i < ignored_late_samples; ++i, pts += period) {
        assert(sc_video_pacing_deadline(&pacing, pts, pts + 20000, delay)
               == pts + 10000);
    }
    for (unsigned i = ignored_late_samples;
            i < SC_VIDEO_PACING_WINDOW - early_samples;
            ++i, pts += period) {
        assert(sc_video_pacing_deadline(&pacing, pts, pts + 20000, delay)
               == pts + 16000);
    }

    assert(sc_video_pacing_deadline(&pacing, pts, pts + 20000, delay)
           == pts + 20000);

    // A source clock reset starts a new cadence.
    assert(!sc_video_pacing_deadline(&pacing, 16000, pts + 30000, delay));
}

int
main(void) {
    test_jitter();
    test_outliers_on_stable_stream();
    test_30_fps_cadence();
    test_sustained_offset_change();
    return 0;
}
