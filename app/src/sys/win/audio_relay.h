#ifndef SC_AUDIO_RELAY_H
#define SC_AUDIO_RELAY_H

#include "common.h"

#include <winsock2.h>
#include <windows.h>
#include "trait/packet_sink.h"

struct sc_audio_relay_buffer;

struct sc_audio_relay {
    HANDLE mutex;
    HANDLE mapping;
    struct sc_audio_relay_buffer *buffer;
    uint64_t generation;
    uint64_t next_packet;
};

struct sc_audio_relay_publisher {
    struct sc_packet_sink sink;
    struct sc_audio_relay relay;
    const char *name;
};

void
sc_audio_relay_publisher_init(struct sc_audio_relay_publisher *publisher,
                              const char *name);

bool
sc_audio_relay_subscribe(struct sc_audio_relay *relay, const char *name,
                         AVCodecParameters *params, AVPacket *config);

// Returns 1 for a packet, 0 if caught up or busy, -1 if the source was lost.
int
sc_audio_relay_read(struct sc_audio_relay *relay, AVPacket *packet);

void
sc_audio_relay_close(struct sc_audio_relay *relay);

#endif
