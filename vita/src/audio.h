#ifndef SNAPVITA_AUDIO_H
#define SNAPVITA_AUDIO_H

#include <stdint.h>

#define AUDIO_SAMPLE_RATE   48000
#define AUDIO_CHANNELS      2
#define AUDIO_GRAIN          960
#define AUDIO_RING_SIZE      (AUDIO_SAMPLE_RATE * AUDIO_CHANNELS * 2 * 4)

typedef struct {
    int port;
    int thread_id;
    int running;

    uint8_t ring[AUDIO_RING_SIZE];
    volatile int write_pos;
    volatile int read_pos;

    int volume_left;
    int volume_right;
    int muted;
} AudioContext;

int  audio_init(AudioContext *ctx);
void audio_fini(AudioContext *ctx);
void audio_start(AudioContext *ctx);
void audio_stop(AudioContext *ctx);

int  audio_write(AudioContext *ctx, const void *data, int size);
int  audio_available(AudioContext *ctx);

void audio_set_volume(AudioContext *ctx, int percent, int muted);

#endif /* SNAPVITA_AUDIO_H */
