#ifndef SNAPVITA_AUDIO_H
#define SNAPVITA_AUDIO_H

#include <stdint.h>

/*
 * Ring buffer sizing:
 * 48000 Hz * 2 ch * 2 bytes = 192000 bytes/sec
 * We allocate enough for ~2 seconds at 48kHz stereo 16-bit.
 * Must be a power of two so we can use bitwise masking for wrap-around.
 */
#define AUDIO_RING_SIZE  (512 * 1024)   /* 512 KB ≈ 2.7s @ 48kHz/stereo/16bit */

/*
 * Grain: samples per sceAudioOutOutput call.
 * 512 = ~10.7 ms @ 48 kHz.  Smaller than 960 (one Snapcast chunk) so each
 * received chunk provides ~1.875 grains — enough slack to absorb normal
 * network jitter without underruns (observed: U=0 in testing).
 */
#define AUDIO_GRAIN  512

typedef struct {
    /* SceAudioOut port, -1 when closed */
    int port;

    /* Format (filled in by audio_configure, before audio_start) */
    int sample_rate;    /* Hz  (e.g. 44100, 48000)  */
    int channels;       /* 1 or 2                    */
    int bits;           /* bits per sample (16)      */
    int bytes_per_frame;/* channels * (bits/8)       */

    /* Preroll: minimum bytes before we allow the thread to output audio */
    int preroll_bytes;
    int prerolled;       /* 1 once preroll_bytes have accumulated */

    /* Playback thread */
    int thread_id;
    volatile int running;

    /* Lock-free ring buffer (single producer / single consumer).
       Positions are signed int32, advanced monotonically; wrap handled
       with masking against (AUDIO_RING_SIZE - 1). */
    uint8_t ring[AUDIO_RING_SIZE];
    volatile int write_pos;   /* written by stream thread  */
    volatile int read_pos;    /* written by audio thread   */

    /* Volume (0..SCE_AUDIO_OUT_MAX_VOL) */
    int volume;
    int muted;

    /* Soft sync: sample rate correction driven by stream thread.
       >0 = drop 1 frame every N frames (speed up, when behind)
       <0 = duplicate 1 frame every N frames (slow down, when ahead)
        0 = no correction */
    volatile int correct_after_x_frames;

    /* Debug counters */
    volatile int stat_underruns;
    volatile int stat_overflows;
    volatile int stat_drops;       /* frames dropped by soft sync */
    volatile int stat_inserts;     /* frames duplicated by soft sync */
} AudioContext;

int  audio_init(AudioContext *ctx);
void audio_fini(AudioContext *ctx);

/*
 * Call this once the codec header has been parsed (before audio_start).
 * Reopens the SceAudioOut port at the correct sample rate / channel count.
 */
int  audio_configure(AudioContext *ctx, int sample_rate, int channels, int bits,
                     int preroll_ms);

void audio_start(AudioContext *ctx);
void audio_stop(AudioContext *ctx);
void audio_flush(AudioContext *ctx);

/*
 * Write PCM data from the streaming thread.
 * If the ring is nearly full, evicts the oldest data to make room so
 * latency stays bounded.  Returns the number of bytes written (always size).
 */
int  audio_write(AudioContext *ctx, const void *data, int size);

/* Current ring buffer fill in bytes */
int  audio_available(AudioContext *ctx);

/* Fill level 0.0 – 1.0 for the GUI progress bar */
float audio_fill_ratio(AudioContext *ctx);

void audio_set_volume(AudioContext *ctx, int percent, int muted);

#endif /* SNAPVITA_AUDIO_H */
