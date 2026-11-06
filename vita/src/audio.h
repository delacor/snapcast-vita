#ifndef SNAPVITA_AUDIO_H
#define SNAPVITA_AUDIO_H

#include <stdint.h>

/*
 * Ring buffer sizing:
 * 48000 Hz * 2 ch * 2 bytes = 192000 bytes/sec
 * Five seconds at the largest supported format, plus a maximum wire chunk.
 * Must be a power of two so we can use bitwise masking for wrap-around.
 */
#define AUDIO_MAX_CHUNK_SIZE (1024 * 1024)
#define AUDIO_RING_SIZE  (2 * AUDIO_MAX_CHUNK_SIZE)

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

    int buffer_ms;       /* effective delay, including format/capacity limits */
    int prerolled;       /* audio thread has reached the scheduled start */
    int scheduled;      /* publishes start_at_usec to the audio thread */
    int needs_resync;    /* consumer underrun: hold silence until reset */
    int64_t start_at_usec; /* local clock; immutable while scheduled */

    /* Playback thread */
    int thread_id;
    volatile int running;
    int output_error;     /* audio thread publishes an output failure */

    /* Single producer / single consumer. Unsigned subtraction handles wrap.
       Only the consumer advances read_pos while running. */
    uint8_t ring[AUDIO_RING_SIZE];
    uint32_t write_pos;       /* written by stream thread  */
    uint32_t read_pos;        /* written by audio thread   */

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

/* Returns zero on success or a negative thread/port error. */
int  audio_start(AudioContext *ctx);
void audio_stop(AudioContext *ctx);
/* Consumer must be stopped, and caller must own the producer. */
void audio_flush(AudioContext *ctx);
int  audio_buffer_ms(const AudioContext *ctx, int64_t requested_ms);
/* Call once after queuing the first chunk following a flush. */
void audio_schedule(AudioContext *ctx, int64_t local_start_usec);

/*
 * Write PCM data from the streaming thread.
 * Accepts a whole, frame-aligned chunk or returns zero without changing the
 * queue. The caller must resynchronize on overflow; no unread data is evicted.
 */
int  audio_write(AudioContext *ctx, const void *data, int size);

/* Current ring buffer fill in bytes */
int  audio_available(AudioContext *ctx);

/* Fill level 0.0 – 1.0 for the GUI progress bar */
float audio_fill_ratio(AudioContext *ctx);

void audio_set_volume(AudioContext *ctx, int percent, int muted);

#endif /* SNAPVITA_AUDIO_H */
