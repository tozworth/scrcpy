#include "audio_relay.h"

#include <stdio.h>
#include <string.h>

#include "util/log.h"

#define SC_AUDIO_RELAY_SLOTS 128
#define SC_AUDIO_RELAY_PACKET_SIZE 32768
#define SC_AUDIO_RELAY_STALE_MS 2000

struct sc_audio_relay_packet {
    int64_t pts;
    int flags;
    int size;
    uint8_t data[SC_AUDIO_RELAY_PACKET_SIZE];
};

// One audio demuxer writes, and any number of recorders read independently.
// A bounded ring keeps recording I/O off the audio playback path.
struct sc_audio_relay_buffer {
    uint64_t generation;
    uint64_t written;
    uint64_t last_packet_at;
    enum AVCodecID codec;
    int format;
    int sample_rate;
    uint64_t channels;
    struct sc_audio_relay_packet config;
    struct sc_audio_relay_packet packets[SC_AUDIO_RELAY_SLOTS];
};

static bool
open_relay(struct sc_audio_relay *relay, const char *name, bool writer) {
    char mutex_name[128];
    if (snprintf(mutex_name, sizeof(mutex_name), "%s.lock", name)
            >= (int) sizeof(mutex_name)) {
        return false;
    }

    HANDLE mutex = writer ? CreateMutexA(NULL, FALSE, mutex_name)
                          : OpenMutexA(SYNCHRONIZE | MUTEX_MODIFY_STATE,
                                       FALSE, mutex_name);
    if (!mutex) {
        return false;
    }

    DWORD access = writer ? FILE_MAP_WRITE : FILE_MAP_READ;
    DWORD size = sizeof(struct sc_audio_relay_buffer);
    HANDLE mapping = writer
        ? CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                             0, size, name)
        : OpenFileMappingA(access, FALSE, name);
    if (!mapping) {
        CloseHandle(mutex);
        return false;
    }
    void *buffer = MapViewOfFile(mapping, access, 0, 0, size);
    CloseHandle(mapping);
    if (!buffer) {
        CloseHandle(mutex);
        return false;
    }

    *relay = (struct sc_audio_relay) { .mutex = mutex, .buffer = buffer };
    return true;
}

void
sc_audio_relay_close(struct sc_audio_relay *relay) {
    UnmapViewOfFile(relay->buffer);
    CloseHandle(relay->mutex);
    relay->buffer = NULL;
}

static bool
source_ready(const struct sc_audio_relay_buffer *buffer) {
    return buffer->codec != AV_CODEC_ID_NONE && buffer->written
        && GetTickCount64() - buffer->last_packet_at < SC_AUDIO_RELAY_STALE_MS;
}

static bool
copy_packet(AVPacket *dst, const struct sc_audio_relay_packet *src) {
    if (src->size <= 0 || src->size > SC_AUDIO_RELAY_PACKET_SIZE
            || av_new_packet(dst, src->size) < 0) {
        return false;
    }
    dst->pts = dst->dts = src->pts;
    dst->flags = src->flags;
    memcpy(dst->data, src->data, src->size);
    return true;
}

bool
sc_audio_relay_subscribe(struct sc_audio_relay *relay, const char *name,
                         AVCodecParameters *params, AVPacket *config) {
    if (!name || !open_relay(relay, name, false)) {
        return false;
    }
    DWORD wait = WaitForSingleObject(relay->mutex, 2);
    bool ready = false;
    const struct sc_audio_relay_buffer *buffer = relay->buffer;
    if (wait == WAIT_OBJECT_0 && source_ready(buffer)) {
        ready = buffer->codec == AV_CODEC_ID_PCM_S16LE
             || copy_packet(config, &buffer->config);
        if (ready) {
            params->codec_type = AVMEDIA_TYPE_AUDIO;
            params->codec_id = buffer->codec;
            params->format = buffer->format;
            params->sample_rate = buffer->sample_rate;
            av_channel_layout_from_mask(&params->ch_layout, buffer->channels);
            relay->generation = buffer->generation;
            // Include audio packets that reached this PC before the first
            // video keyframe. The muxer drops packets older than that frame.
            relay->next_packet = buffer->written
                               - MIN(buffer->written, SC_AUDIO_RELAY_SLOTS);
        }
    }
    if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) {
        ReleaseMutex(relay->mutex);
    }
    if (!ready) {
        sc_audio_relay_close(relay);
    }
    return ready;
}

int
sc_audio_relay_read(struct sc_audio_relay *relay, AVPacket *packet) {
    DWORD wait = WaitForSingleObject(relay->mutex, 0);
    if (wait == WAIT_TIMEOUT) {
        return 0;
    }
    if (wait != WAIT_OBJECT_0) {
        if (wait == WAIT_ABANDONED) {
            ReleaseMutex(relay->mutex);
        }
        return -1;
    }

    int result = 0;
    const struct sc_audio_relay_buffer *buffer = relay->buffer;
    if (buffer->generation != relay->generation || !source_ready(buffer)) {
        result = -1;
    } else if (relay->next_packet < buffer->written) {
        if (buffer->written - relay->next_packet > SC_AUDIO_RELAY_SLOTS) {
            relay->next_packet = buffer->written - SC_AUDIO_RELAY_SLOTS;
        }
        unsigned slot = relay->next_packet++ % SC_AUDIO_RELAY_SLOTS;
        result = copy_packet(packet, &buffer->packets[slot]) ? 1 : -1;
    }
    ReleaseMutex(relay->mutex);
    return result;
}

static bool
publisher_open(struct sc_packet_sink *sink, const AVCodecParameters *params,
               const struct sc_stream_session *session) {
    (void) session;
    struct sc_audio_relay_publisher *publisher =
        container_of(sink, struct sc_audio_relay_publisher, sink);
    struct sc_audio_relay *relay = &publisher->relay;
    if (open_relay(relay, publisher->name, true)) {
        DWORD wait = WaitForSingleObject(relay->mutex, 50);
        if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) {
            struct sc_audio_relay_buffer *buffer = relay->buffer;
            buffer->written = 0;
            buffer->config.size = 0;
            buffer->codec = params->codec_id;
            buffer->format = params->format;
            buffer->sample_rate = params->sample_rate;
            buffer->channels = params->ch_layout.u.mask;
            relay->generation = ++buffer->generation;
            ReleaseMutex(relay->mutex);
            return true;
        }
        sc_audio_relay_close(relay);
    }
    LOGW("Could not share device audio with Appcpy recordings");
    return true; // Audio playback does not depend on recording.
}

static void
publisher_close(struct sc_packet_sink *sink) {
    struct sc_audio_relay_publisher *publisher =
        container_of(sink, struct sc_audio_relay_publisher, sink);
    struct sc_audio_relay *relay = &publisher->relay;
    if (relay->buffer) {
        DWORD wait = WaitForSingleObject(relay->mutex, 10);
        if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) {
            relay->buffer->codec = AV_CODEC_ID_NONE;
            ReleaseMutex(relay->mutex);
        }
        sc_audio_relay_close(relay);
    }
}

static enum sc_sink_result
publisher_push(struct sc_packet_sink *sink, const AVPacket *packet) {
    struct sc_audio_relay_publisher *publisher =
        container_of(sink, struct sc_audio_relay_publisher, sink);
    struct sc_audio_relay *relay = &publisher->relay;
    if (!relay->buffer || packet->size > SC_AUDIO_RELAY_PACKET_SIZE) {
        return SC_SINK_OK;
    }
    DWORD wait = WaitForSingleObject(relay->mutex, 2);
    if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) {
        struct sc_audio_relay_buffer *buffer = relay->buffer;
        bool config = packet->pts == AV_NOPTS_VALUE;
        struct sc_audio_relay_packet *dst = config ? &buffer->config
            : &buffer->packets[buffer->written % SC_AUDIO_RELAY_SLOTS];
        dst->pts = packet->pts;
        dst->flags = packet->flags;
        dst->size = packet->size;
        memcpy(dst->data, packet->data, packet->size);
        if (!config) {
            ++buffer->written;
            buffer->last_packet_at = GetTickCount64();
        }
        ReleaseMutex(relay->mutex);
    }
    return SC_SINK_OK;
}

void
sc_audio_relay_publisher_init(struct sc_audio_relay_publisher *publisher,
                              const char *name) {
    static const struct sc_packet_sink_ops ops = {
        .open = publisher_open,
        .close = publisher_close,
        .push = publisher_push,
    };
    *publisher = (struct sc_audio_relay_publisher) {
        .sink = { .ops = &ops },
        .name = name,
    };
}
