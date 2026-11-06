#include "audio.h"
#include "time_sync.h"
#include "diagnostics.h"
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

#define AUDIO_VOLUME_MAX SCE_AUDIO_OUT_MAX_VOL
#define LOG_FILE         "ux0:data/snapcast/debug.log"
#define LOG_DIR          "ux0:data/snapcast"

/* -----------------------------------------------------------------------
 * Debug logging
 * ----------------------------------------------------------------------- */
#if SNAPVITA_FILE_LOGGING
static void audio_log(const char *fmt, ...) {
    sceIoMkdir(LOG_DIR, 0777);
    SceUID fd = sceIoOpen(LOG_FILE,
                          SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
    if (fd < 0) return;

    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (len > 0) sceIoWrite(fd, buf, len < sizeof(buf) ? len : sizeof(buf) - 1);
    sceIoClose(fd);
}
#else
#define audio_log(...) ((void)0)
#endif

/* -----------------------------------------------------------------------
 * Lock-free ring buffer
 *
 * write_pos / read_pos are monotonically increasing byte counters.
 * Array index = pos & RING_MASK.  AUDIO_RING_SIZE must be a power of two.
 * ----------------------------------------------------------------------- */
#define RING_MASK  (AUDIO_RING_SIZE - 1)

static inline uint32_t atomic_load_acq(const uint32_t *p) {
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}
static inline void atomic_store_rel(uint32_t *p, uint32_t v) {
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

static int ring_readable(AudioContext *ctx) {
    uint32_t w = atomic_load_acq(&ctx->write_pos);
    uint32_t r = atomic_load_acq(&ctx->read_pos);
    uint32_t available = w - r;
    /* A third-party observer (GUI) can sample across both threads' updates. */
    return available <= AUDIO_RING_SIZE ? (int)available : 0;
}

static int ring_read(AudioContext *ctx, void *buf, int size) {
    int avail = ring_readable(ctx);
    int got = avail < size ? avail : size;
    if (got <= 0) return 0;
    uint32_t pos = atomic_load_acq(&ctx->read_pos);
    int r = pos & RING_MASK;
    int first = AUDIO_RING_SIZE - r;
    if (first > got) first = got;
    memcpy(buf, ctx->ring + r, first);
    memcpy((uint8_t *)buf + first, ctx->ring, got - first);
    atomic_store_rel(&ctx->read_pos, pos + (uint32_t)got);
    return got;
}

int audio_write(AudioContext *ctx, const void *data, int size) {
    int bpf = ctx->bytes_per_frame;
    if (!data || size <= 0 || bpf <= 0 || size % bpf != 0 ||
        size > AUDIO_MAX_CHUNK_SIZE || size > AUDIO_RING_SIZE - ring_readable(ctx)) {
        ctx->stat_overflows++;
        return 0;
    }
    uint32_t pos = atomic_load_acq(&ctx->write_pos);
    int w = pos & RING_MASK;
    int first = AUDIO_RING_SIZE - w;
    if (first > size) first = size;
    memcpy(ctx->ring + w, data, first);
    memcpy(ctx->ring, (const uint8_t *)data + first, size - first);
    atomic_store_rel(&ctx->write_pos, pos + (uint32_t)size);
    return size;
}

int audio_buffer_ms(const AudioContext *ctx, int64_t requested_ms) {
    int min_ms = (AUDIO_GRAIN * 4 * 1000 + ctx->sample_rate - 1) / ctx->sample_rate;
    if (min_ms < 50) min_ms = 50;
    /* A burst can append a complete wire chunk while the target delay is
     * already queued. Reserve that space independently of packet duration. */
    int max_ms = (int)((int64_t)(AUDIO_RING_SIZE - AUDIO_MAX_CHUNK_SIZE) * 1000 /
                       (ctx->sample_rate * ctx->bytes_per_frame)) - min_ms;
    if (max_ms > 5000) max_ms = 5000;
    if (requested_ms < min_ms) return min_ms;
    if (requested_ms > max_ms) return max_ms;
    return (int)requested_ms;
}

void audio_schedule(AudioContext *ctx, int64_t local_start_usec) {
    ctx->start_at_usec = local_start_usec;
    __atomic_store_n(&ctx->scheduled, 1, __ATOMIC_RELEASE);
}

int audio_available(AudioContext *ctx) {
    return ring_readable(ctx);
}

float audio_fill_ratio(AudioContext *ctx) {
    float f = (float)ring_readable(ctx) / (float)AUDIO_RING_SIZE;
    if (f < 0.f) f = 0.f;
    if (f > 1.f) f = 1.f;
    return f;
}

/* -----------------------------------------------------------------------
 * Audio playback thread
 *
 * Wait for the timestamp deadline, then drain the ring grain by grain.
 * sceAudioOutOutput() blocks for exactly one grain period, providing pacing.
 * NO sample manipulation here — any arithmetic on int16 values is a bug risk.
 * ----------------------------------------------------------------------- */
#define MAX_FRAME_CORRECTION 32

typedef struct {
    double correction_phase;
    int previous_corr;
} AudioPlayback;

/* Kept separate from platform pacing so the actual consumer can be exercised
 * with a simulated clock. Buffers hold one grain plus correction headroom. */
static void audio_render_grain(AudioContext *ctx, AudioPlayback *playback,
                               void *buf, void *out_buf) {
    int bpf = ctx->bytes_per_frame;
    int frame_bytes = AUDIO_GRAIN * bpf;
    if (__atomic_load_n(&ctx->needs_resync, __ATOMIC_ACQUIRE)) {
        memset(out_buf, 0, frame_bytes);
        return;
    }
    if (!__atomic_load_n(&ctx->prerolled, __ATOMIC_ACQUIRE)) {
        if (!__atomic_load_n(&ctx->scheduled, __ATOMIC_ACQUIRE) ||
            get_tick_usec() < ctx->start_at_usec) {
            memset(out_buf, 0, frame_bytes);
            return;
        }
        /* Wakeup is grain-paced. Trim the elapsed prefix rather than
         * starting an already late chunk from its first sample. */
        int64_t late_usec = get_tick_usec() - ctx->start_at_usec;
        int available_frames = ring_readable(ctx) / bpf;
        int64_t late_frames = late_usec >= (int64_t)available_frames * 1000000 / ctx->sample_rate
                             ? available_frames : late_usec * ctx->sample_rate / 1000000;
        if (late_frames > 0)
            atomic_store_rel(&ctx->read_pos, atomic_load_acq(&ctx->read_pos) +
                             (uint32_t)late_frames * bpf);
        __atomic_store_n(&ctx->prerolled, 1, __ATOMIC_RELEASE);
    }

    int corr = __atomic_load_n(&ctx->correct_after_x_frames, __ATOMIC_ACQUIRE);
    if (corr == 0 || (corr > 0) != (playback->previous_corr > 0))
        playback->correction_phase = 0.0;
    playback->previous_corr = corr;
    if (corr != 0) playback->correction_phase += (double)AUDIO_GRAIN / corr;
    int frames_correction = (int)playback->correction_phase;
    playback->correction_phase -= frames_correction;

    if (frames_correction > MAX_FRAME_CORRECTION)
        frames_correction = MAX_FRAME_CORRECTION;
    if (frames_correction < -MAX_FRAME_CORRECTION)
        frames_correction = -MAX_FRAME_CORRECTION;
    if (frames_correction < 0 && AUDIO_GRAIN + frames_correction < 1)
        frames_correction = -(AUDIO_GRAIN - 1);

    int to_read_frames = AUDIO_GRAIN + frames_correction;
    int to_read_bytes = to_read_frames * bpf;

    int avail = ring_readable(ctx);

    if (avail >= to_read_bytes) {
        ring_read(ctx, buf, to_read_bytes);
    } else if (avail >= frame_bytes) {
        ring_read(ctx, buf, frame_bytes);
        to_read_frames = AUDIO_GRAIN;
        frames_correction = 0;
    } else {
        memset(out_buf, 0, frame_bytes);
        if (avail > 0) ring_read(ctx, out_buf, avail);
        ctx->stat_underruns++;
        __atomic_store_n(&ctx->correct_after_x_frames, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&ctx->needs_resync, 1, __ATOMIC_RELEASE);
        return;
    }

    if (frames_correction == 0) {
        memcpy(out_buf, buf, frame_bytes);
    } else {
        /* Distribute frame drops/duplicates evenly across the grain,
           mirroring the C++ client's "slices" approach. */
        int abs_corr = frames_correction < 0 ? -frames_correction : frames_correction;
        int max_f = frames_correction < 0 ? AUDIO_GRAIN : to_read_frames;
        int slices = abs_corr + 1;
        if (slices > max_f) slices = max_f;
        int slice_size = max_f / slices;
        int pos = 0;

        for (int n = 0; n < slices; n++) {
            int sz = (n + 1 == slices) ? (max_f - pos) : slice_size;

            if (frames_correction > 0) {
                /* Drop: read all input, skip 1 frame per slice in output */
                memcpy((char *)out_buf + (pos - n) * bpf,
                       (char *)buf + pos * bpf,
                       sz * bpf);
            } else {
                /* Duplicate: read fewer input, duplicate 1 frame per slice in output */
                memcpy((char *)out_buf + pos * bpf,
                       (char *)buf + (pos - n) * bpf,
                       sz * bpf);
            }
            pos += sz;
        }

        if (frames_correction > 0)
            ctx->stat_drops += abs_corr;
        else
            ctx->stat_inserts += abs_corr;
    }
}

static int audio_thread(SceSize args, void *argp) {
    AudioContext *ctx = *(AudioContext **)argp;
    /* At most stereo 16-bit frames, comfortably within the 64 KiB stack.
     * No allocation can fail after audio_start has reported success. */
    uint32_t buf[AUDIO_GRAIN + MAX_FRAME_CORRECTION];
    uint32_t out_buf[AUDIO_GRAIN];
    AudioPlayback playback = {0};
    /* Keep filesystem I/O out of the playback loop. Even with a full PCM
     * ring, a slow debug-log write delays the next hardware submission and
     * produces a dropout that the ring-underrun counter cannot detect.
     * The GUI reads the counters; the stream thread provides sync logs. */
    while (__atomic_load_n(&ctx->running, __ATOMIC_ACQUIRE)) {
        audio_render_grain(ctx, &playback, buf, out_buf);
        int result = sceAudioOutOutput(ctx->port, out_buf);
        if (result < 0) {
            __atomic_store_n(&ctx->output_error, result, __ATOMIC_RELEASE);
            break;
        }
    }
    __atomic_store_n(&ctx->running, 0, __ATOMIC_RELEASE);
    return 0;
}

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */

int audio_init(AudioContext *ctx) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->port        = -1;
    ctx->thread_id   = -1;
    ctx->sample_rate = 48000;
    ctx->channels    = 2;
    ctx->bits        = 16;
    ctx->bytes_per_frame = 4;  /* 2ch * 2 bytes */
    ctx->volume      = AUDIO_VOLUME_MAX;

#if SNAPVITA_FILE_LOGGING
    /* Truncate the log file only in an explicitly enabled diagnostic build. */
    sceIoMkdir(LOG_DIR, 0777);
    SceUID fd = sceIoOpen(LOG_FILE,
                          SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd >= 0) sceIoClose(fd);
#endif

    return 0;
}

int audio_configure(AudioContext *ctx, int sample_rate, int channels, int bits,
                    int preroll_ms) {
    /* Stop and release any existing port */
    audio_stop(ctx);
    if (ctx->port >= 0) { sceAudioOutReleasePort(ctx->port); ctx->port = -1; }

    /* Validate & clamp */
    static const int valid_rates[] = {
        8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000, 0
    };
    int rate_ok = 0;
    for (int i = 0; valid_rates[i]; i++)
        if (sample_rate == valid_rates[i]) { rate_ok = 1; break; }

    /* Never reinterpret unsupported PCM as another sample format. */
    if (!rate_ok || (channels != 1 && channels != 2) || bits != 16) {
        audio_log("[audio] unsupported format: %d Hz / %d ch / %d bit\n",
                  sample_rate, channels, bits);
        return -1;
    }
    ctx->sample_rate = sample_rate;
    ctx->channels = channels;
    ctx->bits = bits;
    ctx->bytes_per_frame = channels * (bits / 8);
    ctx->buffer_ms = audio_buffer_ms(ctx, preroll_ms);
    audio_flush(ctx);

    int mode = (channels == 2) ? SCE_AUDIO_OUT_MODE_STEREO
                                : SCE_AUDIO_OUT_MODE_MONO;

    /*
     * Use the BGM port rather than the MAIN (voice) port.
     * MAIN routes through the hardware voice mixer which may apply DSP
     * effects (limiter, spatial audio, EQ) that corrupt transparent PCM
     * playback.  BGM is a simpler direct-to-DAC path with no processing.
     */
    int port = sceAudioOutOpenPort(
        SCE_AUDIO_OUT_PORT_TYPE_BGM,
        AUDIO_GRAIN,
        sample_rate,
        mode
    );

    audio_log("[audio] configure: rate=%d ch=%d bits=%d requested=%dms "
              "effective=%dms grain=%d port=%d\n",
              sample_rate, channels, bits, preroll_ms,
              ctx->buffer_ms, AUDIO_GRAIN, port);

    if (port < 0) return port;
    ctx->port = port;

    /* Restore volume */
    int v = ctx->muted ? 0 : ctx->volume;
    int vol[2] = { v, v };
    sceAudioOutSetVolume(ctx->port,
        SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vol);

    return 0;
}

void audio_fini(AudioContext *ctx) {
    audio_stop(ctx);
    if (ctx->port >= 0) {
        sceAudioOutReleasePort(ctx->port);
        ctx->port = -1;
    }
}

void audio_flush(AudioContext *ctx) {
    uint32_t w = atomic_load_acq(&ctx->write_pos);
    atomic_store_rel(&ctx->read_pos, w);
    __atomic_store_n(&ctx->prerolled, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&ctx->scheduled, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&ctx->needs_resync, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&ctx->correct_after_x_frames, 0, __ATOMIC_RELEASE);
    ctx->stat_underruns = 0;
    ctx->stat_overflows = 0;
    ctx->stat_drops = 0;
    ctx->stat_inserts = 0;
}

int audio_start(AudioContext *ctx) {
    if (__atomic_load_n(&ctx->running, __ATOMIC_ACQUIRE)) return 0;
    audio_stop(ctx); /* Reap a thread that exited after an output failure. */
    if (ctx->port < 0) return -1;
    __atomic_store_n(&ctx->output_error, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&ctx->running, 1, __ATOMIC_RELEASE);

    ctx->thread_id = sceKernelCreateThread(
        "audio_out", audio_thread,
        0x10000100,   /* realtime priority */
        0x10000,      /* 64 KB stack        */
        0, 0, NULL
    );
    if (ctx->thread_id >= 0) {
        AudioContext *arg = ctx;
        int result = sceKernelStartThread(ctx->thread_id, sizeof(arg), &arg);
        if (result < 0) {
            audio_log("[audio] thread start failed: %d\n", result);
            __atomic_store_n(&ctx->running, 0, __ATOMIC_RELEASE);
            sceKernelDeleteThread(ctx->thread_id);
            ctx->thread_id = -1;
            return result;
        }
        audio_log("[audio] thread started\n");
    } else {
        audio_log("[audio] thread create failed: %d\n", ctx->thread_id);
        __atomic_store_n(&ctx->running, 0, __ATOMIC_RELEASE);
        int result = ctx->thread_id;
        ctx->thread_id = -1;
        return result;
    }
    return 0;
}

void audio_stop(AudioContext *ctx) {
    __atomic_store_n(&ctx->running, 0, __ATOMIC_RELEASE);
    if (ctx->thread_id < 0) return;
    sceKernelWaitThreadEnd(ctx->thread_id, NULL, NULL);
    sceKernelDeleteThread(ctx->thread_id);
    ctx->thread_id = -1;
    audio_log("[audio] thread stopped  U=%d  O=%d\n",
              ctx->stat_underruns, ctx->stat_overflows);
}

void audio_set_volume(AudioContext *ctx, int percent, int muted) {
    ctx->muted  = muted;
    ctx->volume = AUDIO_VOLUME_MAX * percent / 100;
    if (ctx->port < 0) return;

    int v = ctx->muted ? 0 : ctx->volume;
    int vol[2] = { v, v };
    sceAudioOutSetVolume(ctx->port,
        SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vol);
}
