#ifndef SC_LIVE_RECORDING_H
#define SC_LIVE_RECORDING_H

#include "common.h"

#include "audio_relay.h"
#include "recorder.h"

struct sc_controller;
struct sc_screen;

enum sc_live_recording_mode {
    SC_LIVE_RECORD_COMBINED,
    SC_LIVE_RECORD_VIDEO,
    SC_LIVE_RECORD_AUDIO,
};

struct sc_live_recording {
    struct sc_packet_sink sink;
    struct sc_controller *controller;
    const char *audio_name;
    enum sc_orientation orientation;

    HANDLE input;
    HANDLE quit;
    sc_thread thread;
    sc_mutex mutex;
    AVCodecParameters *video_params;
    AVPacket *video_config;
    bool video_open;

    // Protected by mutex. The video demuxer can start and feed the muxer.
    // The command thread detaches it before stopping and joining it.
    char *filename;
    char *partial;
    char *screenshot;
    enum sc_live_recording_mode mode;
    bool muxing;
    bool resized;
    bool packet_error;
    int64_t first_video_pts;

    struct sc_recorder muxer;
    atomic_int muxer_result;
    struct sc_audio_relay audio;
    bool audio_queued;
};

bool
sc_live_recording_init(struct sc_live_recording *live,
                       struct sc_controller *controller, const char *audio_name,
                       enum sc_orientation orientation);

void
sc_live_recording_screenshot(struct sc_live_recording *live,
                             struct sc_screen *screen);

void
sc_live_recording_stop(struct sc_live_recording *live);

void
sc_live_recording_destroy(struct sc_live_recording *live);

#endif
