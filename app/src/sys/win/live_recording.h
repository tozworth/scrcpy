#ifndef SC_LIVE_RECORDING_H
#define SC_LIVE_RECORDING_H

#include "common.h"

#include "audio_relay.h"
#include "recorder.h"

struct sc_controller;

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
    bool muxing;
    bool resized;
    bool packet_error;
    int64_t first_video_pts;

    struct sc_recorder muxer;
    atomic_int muxer_result;
    struct sc_audio_relay audio;
    bool with_audio;
};

bool
sc_live_recording_init(struct sc_live_recording *live,
                       struct sc_controller *controller, const char *audio_name,
                       enum sc_orientation orientation);

void
sc_live_recording_stop(struct sc_live_recording *live);

void
sc_live_recording_destroy(struct sc_live_recording *live);

#endif
