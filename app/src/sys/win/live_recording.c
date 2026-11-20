#include "live_recording.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "controller.h"
#include "events.h"
#include "screen.h"
#include "util/log.h"
#include "util/str.h"

#define SC_LIVE_KEYFRAME_TIMEOUT_MS 12000
#define SC_LIVE_COMMAND_SIZE 32768

static void
publish_status(const char *status) {
    // Machine-readable replies are independent of the configured log level.
    printf("APPCPY_RECORDING %s\n", status);
    fflush(stdout);
}

static void
muxer_started(void *userdata) {
    struct sc_live_recording *live = userdata;
    bool missing_audio = live->mode == SC_LIVE_RECORD_COMBINED
                      && !live->muxer.audio;
    publish_status(missing_audio ? "video-only" : "active");
}

static void
muxer_ended(struct sc_recorder *recorder, bool success, void *userdata) {
    (void) recorder;
    struct sc_live_recording *live = userdata;
    atomic_store(&live->muxer_result, success ? 1 : -1);
}

static bool
needs_video_config(enum AVCodecID codec) {
    return codec != AV_CODEC_ID_VP8 && codec != AV_CODEC_ID_VP9;
}

static const char *
start_muxer(struct sc_live_recording *live, const AVPacket *keyframe) {
    static const struct sc_recorder_callbacks callbacks = {
        .on_started = muxer_started,
        .on_ended = muxer_ended,
    };
    bool video = keyframe != NULL;
    bool want_audio = live->mode != SC_LIVE_RECORD_VIDEO;
    AVCodecParameters *audio_params =
        want_audio ? avcodec_parameters_alloc() : NULL;
    AVPacket *audio_config = want_audio ? av_packet_alloc() : NULL;
    bool with_audio = audio_params && audio_config
        && sc_audio_relay_subscribe(&live->audio, live->audio_name,
                                     audio_params, audio_config, video);
    const char *failure = "failed";
    const char *extension = NULL;
    enum sc_record_format format = SC_RECORD_FORMAT_MKV;
    if (!video) {
        if (!with_audio) {
            failure = "audio-unavailable";
            goto end;
        }
        switch (audio_params->codec_id) {
            case AV_CODEC_ID_OPUS:
                format = SC_RECORD_FORMAT_OPUS;
                extension = ".opus";
                break;
            case AV_CODEC_ID_AAC:
                format = SC_RECORD_FORMAT_M4A;
                extension = ".m4a";
                break;
            case AV_CODEC_ID_FLAC:
                format = SC_RECORD_FORMAT_FLAC;
                extension = ".flac";
                break;
            case AV_CODEC_ID_PCM_S16LE:
                format = SC_RECORD_FORMAT_WAV;
                extension = ".wav";
                break;
            default:
                goto end;
        }
        char *filename = sc_str_concat(live->filename, extension);
        if (!filename) {
            goto end;
        }
        free(live->filename);
        live->filename = filename;
        printf("APPCPY_RECORDING audio-extension %s\n", extension);
        fflush(stdout);
    }

    struct sc_recorder *muxer = &live->muxer;
    bool initialized = sc_recorder_init(muxer, live->partial,
                                        format, video, with_audio, live->orientation,
                                        &callbacks, live);
    if (initialized) {
        muxer->live = true;
        muxer->live_started_at = sc_tick_now();
        struct sc_packet_sink *video_sink = &muxer->video_packet_sink;
        struct sc_packet_sink *audio = &muxer->audio_packet_sink;
        bool ok = (!video
              || (video_sink->ops->open(video_sink, live->video_params, NULL)
                  && (!needs_video_config(live->video_params->codec_id)
                      || video_sink->ops->push(video_sink, live->video_config)
                          == SC_SINK_OK)
                  && video_sink->ops->push(video_sink, keyframe) == SC_SINK_OK))
          && (!with_audio
              || (audio->ops->open(audio, audio_params, NULL)
                  && (!audio_config->size
                      || audio->ops->push(audio, audio_config) == SC_SINK_OK)))
          && sc_recorder_start(muxer);
        if (ok) {
            live->first_video_pts = video ? keyframe->pts : INT64_MIN;
            live->muxing = true;
            failure = NULL;
        } else {
            sc_recorder_destroy(muxer);
        }
    }

end:
    avcodec_parameters_free(&audio_params);
    av_packet_free(&audio_config);
    if (failure && live->audio.buffer) {
        sc_audio_relay_close(&live->audio);
    }
    return failure;
}

static bool
publish_file(const char *source, const char *destination) {
    wchar_t *partial = sc_str_to_wchars(source);
    wchar_t *filename = sc_str_to_wchars(destination);
    bool ok = partial && filename && MoveFileW(partial, filename);
    if (!ok) {
        LOGE("Could not move recording %s to %s", source, destination);
    }
    free(partial);
    free(filename);
    return ok;
}

static void
remove_partial_file(const char *filename) {
    wchar_t *path = sc_str_to_wchars(filename);
    if (path) {
        DeleteFileW(path);
        free(path);
    }
}

static void
finish_recording(struct sc_live_recording *live, const char *inactive_status) {
    sc_mutex_lock(&live->mutex);
    bool muxing = live->muxing;
    bool resized = live->resized;
    bool packet_error = live->packet_error;
    live->muxing = false;
    // Detach the video tap before joining the writer. A new request can only
    // be received by this thread, after finalization completes.
    char *filename = live->filename;
    live->filename = NULL;
    sc_mutex_unlock(&live->mutex);
    if (!filename) {
        return;
    }

    const char *status = inactive_status;
    int64_t duration_ms = -1;
    if (muxing) {
        sc_recorder_stop(&live->muxer);
        sc_recorder_join(&live->muxer);
        sc_tick elapsed = live->muxer.live_stopped_at - live->muxer.live_started_at;
        sc_recorder_destroy(&live->muxer);
        if (live->audio.buffer) {
            sc_audio_relay_close(&live->audio);
        }
        if (live->mode == SC_LIVE_RECORD_AUDIO && !live->audio_queued) {
            remove_partial_file(live->partial);
            status = "failed";
        } else if (atomic_load(&live->muxer_result) == 1) {
            // Use an atomic rename in the same directory so the final path
            // appears only after the trailer is written, and never overwrite.
            bool saved = publish_file(live->partial, filename);
            status = !saved ? "failed" : resized ? "resized"
                   : packet_error ? "interrupted" : "saved";
            if (saved) {
                duration_ms = SC_TICK_TO_MS(MAX(0, elapsed));
            }
        } else {
            status = "failed";
        }
    } else if (packet_error) {
        status = "failed";
    }
    free(filename);
    free(live->partial);
    live->partial = NULL;
    if (duration_ms >= 0) {
        printf("APPCPY_RECORDING %s %" PRIi64 "\n", status, duration_ms);
        fflush(stdout);
    } else {
        publish_status(status);
    }
}

static void
request_screenshot(struct sc_live_recording *live, const char *path) {
    sc_mutex_lock(&live->mutex);
    if (!live->screenshot) {
        live->screenshot = strdup(path);
        if (!live->screenshot
                || !sc_push_event_with_data(SC_EVENT_APPCPY_SCREENSHOT, live)) {
            free(live->screenshot);
            live->screenshot = NULL;
            publish_status("screenshot-failed");
        }
    }
    sc_mutex_unlock(&live->mutex);
}

void
sc_live_recording_screenshot(struct sc_live_recording *live,
                             struct sc_screen *screen) {
    sc_mutex_lock(&live->mutex);
    char *filename = live->screenshot;
    live->screenshot = NULL;
    sc_mutex_unlock(&live->mutex);

    char *partial = sc_str_concat(filename, ".partial");
    bool saved = partial && sc_screen_save_screenshot(screen, partial)
              && publish_file(partial, filename);
    if (!saved && partial) {
        remove_partial_file(partial);
    }
    free(partial);
    free(filename);
    publish_status(saved ? "screenshot-saved" : "screenshot-failed");
}

static void
request_keyframe(struct sc_live_recording *live, bool restart) {
    if (live->controller) {
        struct sc_control_msg msg = {
            .type = SC_CONTROL_MSG_TYPE_REQUEST_VIDEO_KEYFRAME,
            .restart_video_for_keyframe = restart,
        };
        sc_controller_push_msg(live->controller, &msg);
    }
}

static bool
handle_command(struct sc_live_recording *live, const char *command) {
    if (!strcmp(command, "stop")) {
        finish_recording(live, "canceled");
        return false;
    }
    if (!strncmp(command, "screenshot ", 11) && command[11]) {
        request_screenshot(live, command + 11);
        return false;
    }

    enum sc_live_recording_mode mode;
    const char *filename;
    if (!strncmp(command, "record ", 7)) {
        mode = SC_LIVE_RECORD_COMBINED;
        filename = command + 7;
    } else if (!strncmp(command, "record-video ", 13)) {
        mode = SC_LIVE_RECORD_VIDEO;
        filename = command + 13;
    } else if (!strncmp(command, "record-audio ", 13)) {
        mode = SC_LIVE_RECORD_AUDIO;
        filename = command + 13;
    } else {
        return false;
    }
    if (!*filename) {
        return false;
    }

    sc_mutex_lock(&live->mutex);
    if (live->filename) {
        sc_mutex_unlock(&live->mutex);
        return false;
    }
    char *path = strdup(filename);
    char *partial = sc_str_concat(filename, ".partial");
    if (!path || !partial) {
        free(path);
        free(partial);
        sc_mutex_unlock(&live->mutex);
        publish_status("failed");
        return false;
    }
    live->filename = path;
    live->partial = partial;
    live->mode = mode;
    live->resized = false;
    live->packet_error = false;
    live->audio_queued = false;
    atomic_store(&live->muxer_result, 0);
    const char *failure =
        mode == SC_LIVE_RECORD_AUDIO ? start_muxer(live, NULL) : NULL;
    sc_mutex_unlock(&live->mutex);
    if (mode == SC_LIVE_RECORD_AUDIO) {
        if (failure) {
            finish_recording(live, failure);
        }
        return false;
    }
    request_keyframe(live, false);
    return true;
}

static void
forward_audio(struct sc_live_recording *live) {
    struct sc_packet_sink *sink = &live->muxer.audio_packet_sink;
    // Bound the work per iteration so stop and shutdown take precedence.
    for (unsigned i = 0; i < 128; ++i) {
        AVPacket packet = {0};
        int read = sc_audio_relay_read(&live->audio, &packet);
        if (!read) {
            break;
        }
        if (read < 0) {
            if (live->mode == SC_LIVE_RECORD_AUDIO) {
                sc_mutex_lock(&live->mutex);
                live->packet_error = true;
                sc_mutex_unlock(&live->mutex);
            } else {
                sc_audio_relay_close(&live->audio);
                publish_status("audio-ended");
            }
            break;
        }
        enum sc_sink_result result = SC_SINK_OK;
        if (packet.pts >= live->first_video_pts) {
            result = sink->ops->push(sink, &packet);
            if (result == SC_SINK_OK) {
                live->audio_queued = true;
            }
        }
        av_packet_unref(&packet);
        if (result != SC_SINK_OK) {
            sc_mutex_lock(&live->mutex);
            live->packet_error = true;
            sc_mutex_unlock(&live->mutex);
            break;
        }
    }
}

static int
recording_commands(void *data) {
    struct sc_live_recording *live = data;
    char command[SC_LIVE_COMMAND_SIZE];
    size_t len = 0;
    uint64_t requested_at = 0;
    bool retried = false;
    bool muxing = false;
    while (WaitForSingleObject(live->quit, muxing ? 10 : 25) == WAIT_TIMEOUT) {
        DWORD available;
        if (!PeekNamedPipe(live->input, NULL, 0, NULL, &available, NULL)) {
            break;
        }
        if (available) {
            char bytes[4096];
            DWORD count;
            if (!ReadFile(live->input, bytes, MIN(available, sizeof(bytes)),
                          &count, NULL) || !count) {
                break;
            }
            for (DWORD i = 0; i < count; ++i) {
                if (bytes[i] == '\n') {
                    if (len < sizeof(command)) {
                        if (len && command[len - 1] == '\r') {
                            --len;
                        }
                        command[len] = '\0';
                        if (handle_command(live, command)) {
                            requested_at = GetTickCount64();
                            retried = false;
                        }
                    }
                    len = 0;
                } else if (len < sizeof(command)) {
                    command[len++] = bytes[i];
                }
            }
        }

        sc_mutex_lock(&live->mutex);
        muxing = live->muxing;
        bool requested = live->filename;
        bool finish = live->resized || live->packet_error;
        sc_mutex_unlock(&live->mutex);
        if (!requested) {
            continue;
        }
        if (finish || atomic_load(&live->muxer_result)) {
            finish_recording(live, "failed");
        } else if (muxing) {
            if (live->audio.buffer) {
                forward_audio(live);
            }
        } else {
            uint64_t elapsed = GetTickCount64() - requested_at;
            if (elapsed >= SC_LIVE_KEYFRAME_TIMEOUT_MS) {
                finish_recording(live, "no-keyframe");
            } else if (elapsed >= 1000 && !retried) {
                retried = true;
                request_keyframe(live, true);
            }
        }
    }
    finish_recording(live, "canceled");
    return 0;
}

static bool
video_open(struct sc_packet_sink *sink, const AVCodecParameters *params,
           const struct sc_stream_session *session) {
    (void) session;
    struct sc_live_recording *live =
        container_of(sink, struct sc_live_recording, sink);
    sc_mutex_lock(&live->mutex);
    live->video_open = avcodec_parameters_copy(live->video_params, params) >= 0;
    sc_mutex_unlock(&live->mutex);
    return true;
}

static void
video_close(struct sc_packet_sink *sink) {
    struct sc_live_recording *live =
        container_of(sink, struct sc_live_recording, sink);
    sc_mutex_lock(&live->mutex);
    live->video_open = false;
    sc_mutex_unlock(&live->mutex);
}

static enum sc_sink_result
video_session(struct sc_packet_sink *sink,
              const struct sc_stream_session *session) {
    struct sc_live_recording *live =
        container_of(sink, struct sc_live_recording, sink);
    sc_mutex_lock(&live->mutex);
    int width = session->video.width;
    int height = session->video.height;
    if (live->video_params->width != width
            || live->video_params->height != height) {
        // Matroska declares the dimensions once for the track. Finish the
        // current file before accepting samples from a different size.
        live->resized = live->muxing && live->mode != SC_LIVE_RECORD_AUDIO;
        live->video_params->width = width;
        live->video_params->height = height;
    }
    // A new encoder session supplies fresh codec data before its keyframe.
    av_packet_unref(live->video_config);
    sc_mutex_unlock(&live->mutex);
    return SC_SINK_OK;
}

static enum sc_sink_result
video_push(struct sc_packet_sink *sink, const AVPacket *packet) {
    struct sc_live_recording *live =
        container_of(sink, struct sc_live_recording, sink);
    sc_mutex_lock(&live->mutex);
    bool config = packet->pts == AV_NOPTS_VALUE;
    if (config) {
        av_packet_unref(live->video_config);
        av_packet_ref(live->video_config, packet);
    }
    if (live->filename && live->mode != SC_LIVE_RECORD_AUDIO
            && !live->resized && !live->packet_error) {
        if (live->muxing) {
            struct sc_packet_sink *video = &live->muxer.video_packet_sink;
            live->packet_error = video->ops->push(video, packet) != SC_SINK_OK;
        } else if (!config && (packet->flags & AV_PKT_FLAG_KEY)
                && live->video_open && (live->video_config->size
                    || !needs_video_config(live->video_params->codec_id))) {
            live->packet_error = start_muxer(live, packet) != NULL;
        }
    }
    sc_mutex_unlock(&live->mutex);
    return SC_SINK_OK; // A recording failure must never end the video stream.
}

bool
sc_live_recording_init(struct sc_live_recording *live,
                       struct sc_controller *controller, const char *audio_name,
                       enum sc_orientation orientation) {
    static const struct sc_packet_sink_ops ops = {
        .open = video_open,
        .close = video_close,
        .push = video_push,
        .push_session = video_session,
    };
    *live = (struct sc_live_recording) {
        .sink = { .ops = &ops },
        .audio_name = audio_name,
        .controller = controller,
        .orientation = orientation,
        .input = GetStdHandle(STD_INPUT_HANDLE),
    };
    atomic_init(&live->muxer_result, 0);
    if (GetFileType(live->input) != FILE_TYPE_PIPE || !sc_mutex_init(&live->mutex)) {
        return false;
    }
    live->quit = CreateEventW(NULL, TRUE, FALSE, NULL);
    live->video_params = avcodec_parameters_alloc();
    live->video_config = av_packet_alloc();
    if (live->quit && live->video_params && live->video_config
            && sc_thread_create(&live->thread, recording_commands,
                                "appcpy-record", live)) {
        return true;
    }

    if (live->quit) {
        CloseHandle(live->quit);
    }
    avcodec_parameters_free(&live->video_params);
    av_packet_free(&live->video_config);
    sc_mutex_destroy(&live->mutex);
    return false;
}

void
sc_live_recording_stop(struct sc_live_recording *live) {
    SetEvent(live->quit);
}

void
sc_live_recording_destroy(struct sc_live_recording *live) {
    sc_thread_join(&live->thread, NULL);
    free(live->screenshot);
    CloseHandle(live->quit);
    avcodec_parameters_free(&live->video_params);
    av_packet_free(&live->video_config);
    sc_mutex_destroy(&live->mutex);
}
