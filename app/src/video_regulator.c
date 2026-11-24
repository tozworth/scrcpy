#include "video_regulator.h"

#include <assert.h>
#include <stdlib.h>
#include <libavcodec/avcodec.h>

#ifdef _WIN32
# include <windows.h>
# ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#  define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
# endif
#endif

#include "util/log.h"

/** Downcast frame_sink to sc_video_regulator */
#define DOWNCAST(SINK) container_of(SINK, struct sc_video_regulator, frame_sink)

static bool
sc_delayed_packet_init_frame(struct sc_delayed_packet *dpacket,
                             const AVFrame *frame) {
    dpacket->type = SC_DELAYED_PACKET_TYPE_FRAME;
    dpacket->frame = av_frame_alloc();
    if (!dpacket->frame) {
        LOG_OOM();
        return false;
    }

    if (av_frame_ref(dpacket->frame, frame)) {
        LOG_OOM();
        av_frame_free(&dpacket->frame);
        return false;
    }

    return true;
}

static void
sc_delayed_packet_init_session(struct sc_delayed_packet *dpacket,
                               const struct sc_stream_session *session) {
    dpacket->type = SC_DELAYED_PACKET_TYPE_SESSION;
    dpacket->session = *session;
}

static void
sc_delayed_packet_destroy(struct sc_delayed_packet *dpacket) {
    if (dpacket->type == SC_DELAYED_PACKET_TYPE_FRAME) {
        av_frame_unref(dpacket->frame);
        av_frame_free(&dpacket->frame);
    }
}

static bool
sc_video_regulator_timedwait(struct sc_video_regulator *vr, sc_tick deadline) {
#ifdef _WIN32
    if (vr->pacing_timer) {
        // Automatic deadlines are immutable, so only stop can interrupt this
        // wait. Fixed buffering still uses wait_cond for clock updates.
        HANDLE handles[] = {vr->pacing_stop_event, vr->pacing_timer};
        for (;;) {
            sc_tick remaining = deadline - sc_tick_now();
            if (remaining <= 0) {
                return false;
            }

            LARGE_INTEGER due_time;
            due_time.QuadPart = -10 * remaining; // relative, in 100 ns units
            if (!SetWaitableTimer(vr->pacing_timer, &due_time, 0, NULL,
                                  NULL, FALSE)) {
                break;
            }

            sc_mutex_unlock(&vr->mutex);
            DWORD result = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
            sc_mutex_lock(&vr->mutex);
            if (vr->stopped) {
                return true;
            }
            if (result != WAIT_OBJECT_0 + 1) {
                break;
            }
        }
    }
#endif
    return sc_cond_timedwait(&vr->wait_cond, &vr->mutex, deadline);
}

static int
run_buffering(void *data) {
    struct sc_video_regulator *vr = data;

    assert(vr->delay > 0);

    for (;;) {
        sc_mutex_lock(&vr->mutex);

        while (!vr->stopped && sc_vecdeque_is_empty(&vr->queue)) {
            sc_cond_wait(&vr->queue_cond, &vr->mutex);
        }

        if (vr->stopped) {
            sc_mutex_unlock(&vr->mutex);
            goto stopped;
        }

        struct sc_delayed_packet dpacket = sc_vecdeque_pop(&vr->queue);
        sc_cond_signal(&vr->backpressure_cond);

        enum sc_sink_result result;
        if (dpacket.type == SC_DELAYED_PACKET_TYPE_FRAME) {
            sc_tick max_deadline = sc_tick_now() + vr->delay;
            // PTS (written by the server) are expressed in microseconds
            sc_tick pts = SC_TICK_FROM_US(dpacket.frame->pts);

            bool timed_out = false;
            while (!vr->stopped && !timed_out) {
                sc_tick deadline = dpacket.auto_deadline;
                if (!vr->sparse_frames_asap) {
                    deadline = sc_clock_to_system_time(&vr->clock, pts)
                             + vr->delay;
                    if (deadline > max_deadline) {
                        deadline = max_deadline;
                    }
                }

                timed_out = !sc_video_regulator_timedwait(vr, deadline);
            }

            bool stopped = vr->stopped;
            sc_mutex_unlock(&vr->mutex);

            if (stopped) {
                sc_delayed_packet_destroy(&dpacket);
                goto stopped;
            }

#ifdef SC_BUFFERING_DEBUG
            LOGD("Buffering: %" PRItick ";%" PRItick ";%" PRItick,
                 pts, dpacket.push_date, sc_tick_now());
#endif

            result = sc_frame_source_sinks_push(&vr->frame_source,
                                                dpacket.frame);
        } else {
            assert(dpacket.type == SC_DELAYED_PACKET_TYPE_SESSION);
            sc_mutex_unlock(&vr->mutex);
            result = sc_frame_source_sinks_push_session(&vr->frame_source,
                                                        &dpacket.session);
        }

        sc_delayed_packet_destroy(&dpacket);
        if (result != SC_SINK_OK) {
            if (result == SC_SINK_KO) {
                LOGE("Delayed packet could not be pushed, stopping");
            }
            sc_mutex_lock(&vr->mutex);
            // Prevent to push any new packet
            vr->stopped = true;
            sc_cond_signal(&vr->backpressure_cond);
            sc_mutex_unlock(&vr->mutex);
            goto stopped;
        }
    }

stopped:
    assert(vr->stopped);

    // Flush queue
    while (!sc_vecdeque_is_empty(&vr->queue)) {
        struct sc_delayed_packet *dpacket = sc_vecdeque_popref(&vr->queue);
        sc_delayed_packet_destroy(dpacket);
    }

    LOGD("Buffering thread ended");

    return 0;
}

static bool
sc_video_regulator_frame_sink_open(struct sc_frame_sink *sink,
                                   const AVCodecContext *ctx,
                                   const struct sc_stream_session *session) {
    struct sc_video_regulator *vr = DOWNCAST(sink);
    (void) ctx;
    (void) session;

    sc_clock_init(&vr->clock);
    vr->pacing.count = 0;
    sc_vecdeque_init(&vr->queue);

    if (!sc_frame_source_sinks_open(&vr->frame_source, ctx, session)) {
        return false;
    }

    bool ok = sc_thread_create(&vr->thread, run_buffering, "scrcpy-vruf", vr);
    if (!ok) {
        LOGE("Could not start buffering thread");
        sc_frame_source_sinks_close(&vr->frame_source);
        return false;
    }

    return true;
}

static void
sc_video_regulator_frame_sink_close(struct sc_frame_sink *sink) {
    struct sc_video_regulator *vr = DOWNCAST(sink);

    sc_video_regulator_stop(vr);
    sc_thread_join(&vr->thread, NULL);

    sc_frame_source_sinks_close(&vr->frame_source);

    sc_vecdeque_destroy(&vr->queue);
}

static enum sc_sink_result
sc_video_regulator_frame_sink_push(struct sc_frame_sink *sink,
                                   const AVFrame *frame) {
    struct sc_video_regulator *vr = DOWNCAST(sink);

    sc_mutex_lock(&vr->mutex);

    if (vr->stopped) {
        sc_mutex_unlock(&vr->mutex);
        return SC_SINK_STOPPED;
    }

    assert(frame->opaque_ref);
    assert(frame->opaque_ref->size == sizeof(sc_tick));
    // Retrieve the recv date set on the AVPacket by the demuxer
    sc_tick recv_date = *(const sc_tick *) frame->opaque_ref->data;

    sc_tick pts = SC_TICK_FROM_US(frame->pts);
    if (!vr->sparse_frames_asap) {
        sc_clock_update(&vr->clock, recv_date, pts);
        sc_cond_signal(&vr->wait_cond);
        if (vr->first_frame_asap && vr->clock.range == 1) {
            sc_mutex_unlock(&vr->mutex);
            return sc_frame_source_sinks_push(&vr->frame_source, frame);
        }
    }

    struct sc_delayed_packet dpacket;
    bool ok = sc_delayed_packet_init_frame(&dpacket, frame);
    if (!ok) {
        sc_mutex_unlock(&vr->mutex);
        return SC_SINK_KO;
    }

    dpacket.auto_deadline = vr->sparse_frames_asap
        ? sc_video_pacing_deadline(&vr->pacing, pts, recv_date, vr->delay)
        : 0;

#ifdef SC_BUFFERING_DEBUG
    dpacket.push_date = sc_tick_now();
#endif

    if (!sc_vecdeque_push(&vr->queue, dpacket)) {
        sc_delayed_packet_destroy(&dpacket);
        sc_mutex_unlock(&vr->mutex);
        LOG_OOM();
        return SC_SINK_KO;
    }

    sc_cond_signal(&vr->queue_cond);

    sc_mutex_unlock(&vr->mutex);

    return SC_SINK_OK;
}

static enum sc_sink_result
sc_video_regulator_frame_sink_push_session(struct sc_frame_sink *sink,
                                      const struct sc_stream_session *session) {
    struct sc_video_regulator *vr = DOWNCAST(sink);

    sc_mutex_lock(&vr->mutex);

    if (vr->stopped) {
        sc_mutex_unlock(&vr->mutex);
        return SC_SINK_STOPPED;
    }

    struct sc_delayed_packet *dpacket =
        sc_vecdeque_push_uninitialized(&vr->queue);
    if (!dpacket) {
        sc_mutex_unlock(&vr->mutex);
        LOG_OOM();
        return SC_SINK_KO;
    }

    sc_delayed_packet_init_session(dpacket, session);
    dpacket->auto_deadline = 0;
    vr->pacing.count = 0;

#ifdef SC_BUFFERING_DEBUG
    dpacket->push_date = sc_tick_now();
#endif

    sc_cond_signal(&vr->queue_cond);

    sc_mutex_unlock(&vr->mutex);

    return SC_SINK_OK;
}

static void
sc_video_regulator_frame_sink_apply_backpressure(struct sc_frame_sink *sink) {
    struct sc_video_regulator *vr = DOWNCAST(sink);

    if (!vr->backpressure_threshold) {
        // Backpressure disabled
        return;
    }

    sc_mutex_lock(&vr->mutex);
    // This not only counts queued frames, but also queued session packets.
    // But session packets are rare and consumed quickly, so keep it simple.
    while (!vr->stopped
            && sc_vecdeque_size(&vr->queue) > vr->backpressure_threshold) {
        sc_cond_wait(&vr->backpressure_cond, &vr->mutex);
    }
    sc_mutex_unlock(&vr->mutex);
}

bool
sc_video_regulator_init(struct sc_video_regulator *vr, sc_tick delay,
                        bool first_frame_asap, bool sparse_frames_asap,
                        uint32_t backpressure_threshold) {
    assert(delay > 0);

    bool ok = sc_mutex_init(&vr->mutex);
    if (!ok) {
        return false;
    }

    ok = sc_cond_init(&vr->queue_cond);
    if (!ok) {
        goto error_destroy_mutex;
    }

    ok = sc_cond_init(&vr->wait_cond);
    if (!ok) {
        goto error_destroy_queue_cond;
    }

    ok = sc_cond_init(&vr->backpressure_cond);
    if (!ok) {
        goto error_destroy_wait_cond;
    }

    vr->delay = delay;
    vr->first_frame_asap = first_frame_asap;
    vr->sparse_frames_asap = sparse_frames_asap;
    vr->backpressure_threshold = backpressure_threshold;
    vr->stopped = false;

#ifdef _WIN32
    vr->pacing_timer = NULL;
    vr->pacing_stop_event = NULL;
    if (sparse_frames_asap) {
        vr->pacing_timer =
            CreateWaitableTimerExW(NULL, NULL,
                                   CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                   TIMER_MODIFY_STATE | SYNCHRONIZE);
        if (vr->pacing_timer) {
            vr->pacing_stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
            if (!vr->pacing_stop_event) {
                CloseHandle(vr->pacing_timer);
                vr->pacing_timer = NULL;
            }
        }
    }
#endif

    sc_frame_source_init(&vr->frame_source);

    static const struct sc_frame_sink_ops ops = {
        .open = sc_video_regulator_frame_sink_open,
        .close = sc_video_regulator_frame_sink_close,
        .push = sc_video_regulator_frame_sink_push,
        .push_session = sc_video_regulator_frame_sink_push_session,
        .apply_backpressure = sc_video_regulator_frame_sink_apply_backpressure,
    };

    vr->frame_sink.ops = &ops;

    return true;

error_destroy_wait_cond:
    sc_cond_destroy(&vr->wait_cond);
error_destroy_queue_cond:
    sc_cond_destroy(&vr->queue_cond);
error_destroy_mutex:
    sc_mutex_destroy(&vr->mutex);

    return false;
}

void
sc_video_regulator_stop(struct sc_video_regulator *vr) {
    sc_mutex_lock(&vr->mutex);
    vr->stopped = true;
#ifdef _WIN32
    if (vr->pacing_stop_event) {
        SetEvent(vr->pacing_stop_event);
    }
#endif
    sc_cond_signal(&vr->queue_cond);
    sc_cond_signal(&vr->wait_cond);
    sc_cond_signal(&vr->backpressure_cond);
    sc_mutex_unlock(&vr->mutex);
}

void
sc_video_regulator_destroy(struct sc_video_regulator *vr) {
#ifdef _WIN32
    if (vr->pacing_timer) {
        CloseHandle(vr->pacing_timer);
        CloseHandle(vr->pacing_stop_event);
    }
#endif
    sc_cond_destroy(&vr->backpressure_cond);
    sc_cond_destroy(&vr->wait_cond);
    sc_cond_destroy(&vr->queue_cond);
    sc_mutex_destroy(&vr->mutex);
}
