#include "audio.h"
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
    sceIoWrite(fd, buf, len);
    sceIoClose(fd);
}

/* -----------------------------------------------------------------------
 * Lock-free ring buffer
 *
 * write_pos / read_pos are monotonically increasing byte counters.
 * Array index = pos & RING_MASK.  AUDIO_RING_SIZE must be a power of two.
 * ----------------------------------------------------------------------- */
#define RING_MASK  (AUDIO_RING_SIZE - 1)

static inline int atomic_load_acq(volatile int *p) {
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}
static inline void atomic_store_rel(volatile int *p, int v) {
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

/* Bytes currently readable (written but not yet consumed) */
static int ring_readable(AudioContext *ctx) {
    int w = atomic_load_acq(&ctx->write_pos);
    int r = atomic_load_acq(&ctx->read_pos);
    return w - r;
}

/* Free bytes available to write */
static int ring_writable(AudioContext *ctx) {
    return AUDIO_RING_SIZE - ring_readable(ctx);
}

/* Read `size` bytes from the ring into `buf`.  Returns bytes actually read. */
static int ring_read(AudioContext *ctx, void *buf, int size) {
    int avail = ring_readable(ctx);
    int got   = (avail < size) ? avail : size;
    if (got <= 0) return 0;

    int r = atomic_load_acq(&ctx->read_pos) & RING_MASK;

    if (r + got <= AUDIO_RING_SIZE) {
        memcpy(buf, ctx->ring + r, got);
    } else {
        int first = AUDIO_RING_SIZE - r;
        memcpy(buf, ctx->ring + r, first);
        memcpy((uint8_t *)buf + first, ctx->ring, got - first);
    }

    atomic_store_rel(&ctx->read_pos,
                     atomic_load_acq(&ctx->read_pos) + got);
    return got;
}

/* -----------------------------------------------------------------------
 * Write PCM data from the streaming thread.
 * Evicts OLDEST data when the ring is full so latency stays bounded.
 * ----------------------------------------------------------------------- */
int audio_write(AudioContext *ctx, const void *data, int size) {
    if (size <= 0) return 0;

    int space = ring_writable(ctx);
    if (size > space) {
        /* Drop oldest samples, aligned to frame boundary */
        int drop = size - space;
        int bpf  = (ctx->bytes_per_frame > 0) ? ctx->bytes_per_frame : 4;
        drop     = ((drop + bpf - 1) / bpf) * bpf;
        atomic_store_rel(&ctx->read_pos,
                         atomic_load_acq(&ctx->read_pos) + drop);
        ctx->stat_overflows++;
    }

    int w = atomic_load_acq(&ctx->write_pos) & RING_MASK;

    if (w + size <= AUDIO_RING_SIZE) {
        memcpy(ctx->ring + w, data, size);
    } else {
        int first = AUDIO_RING_SIZE - w;
        memcpy(ctx->ring + w, data, first);
        memcpy(ctx->ring, (const uint8_t *)data + first, size - first);
    }

    atomic_store_rel(&ctx->write_pos,
                     atomic_load_acq(&ctx->write_pos) + size);

    /* Trip the preroll latch once when enough data has accumulated */
    if (!__atomic_load_n(&ctx->prerolled, __ATOMIC_ACQUIRE) &&
        ring_readable(ctx) >= ctx->preroll_bytes) {
        __atomic_store_n(&ctx->prerolled, 1, __ATOMIC_RELEASE);
    }

    return size;
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
 * Deliberately simple: wait for preroll, then drain the ring frame by frame.
 * sceAudioOutOutput() blocks for exactly one grain period, providing pacing.
 * NO sample manipulation here — any arithmetic on int16 values is a bug risk.
 * ----------------------------------------------------------------------- */
static int audio_thread(SceSize args, void *argp) {
    AudioContext *ctx = *(AudioContext **)argp;

    int frame_bytes = AUDIO_GRAIN * ctx->channels * (ctx->bits / 8);

    /* Allocate output buffer on heap — not on the 64 KB thread stack */
    void *buf = malloc(frame_bytes);
    if (!buf) return -1;

    int stat_period = 0;

    while (__atomic_load_n(&ctx->running, __ATOMIC_ACQUIRE)) {

        /* --- Pre-roll: output silence until enough data has buffered --- */
        if (!__atomic_load_n(&ctx->prerolled, __ATOMIC_ACQUIRE)) {
            memset(buf, 0, frame_bytes);
            sceAudioOutOutput(ctx->port, buf);
            continue;
        }

        /* --- Normal / underrun path --- */
        int avail = ring_readable(ctx);

        if (avail >= frame_bytes) {
            ring_read(ctx, buf, frame_bytes);
        } else {
            /*
             * Underrun: drain whatever remains and zero-pad to a full grain.
             * Zeroing the whole buffer first guarantees clean silence even
             * if ring_read returns fewer bytes than expected.
             */
            memset(buf, 0, frame_bytes);
            if (avail > 0) ring_read(ctx, buf, avail);
            ctx->stat_underruns++;
        }

        sceAudioOutOutput(ctx->port, buf);

        /* Periodic diagnostic log (every ~5 s worth of frames) */
        stat_period++;
        if (stat_period >= 500) {
            stat_period = 0;
            audio_log("[audio] fill=%d/%d  U=%d  O=%d\n",
                      ring_readable(ctx), AUDIO_RING_SIZE,
                      ctx->stat_underruns, ctx->stat_overflows);
        }
    }

    free(buf);
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

    /* Truncate the log file at startup */
    sceIoMkdir(LOG_DIR, 0777);
    SceUID fd = sceIoOpen(LOG_FILE,
                          SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd >= 0) sceIoClose(fd);

    return 0;
}

int audio_configure(AudioContext *ctx, int sample_rate, int channels, int bits,
                    int preroll_ms) {
    /* Stop and release any existing port */
    if (ctx->running)   audio_stop(ctx);
    if (ctx->port >= 0) { sceAudioOutReleasePort(ctx->port); ctx->port = -1; }

    /* Validate & clamp */
    static const int valid_rates[] = {
        8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000, 0
    };
    int rate_ok = 0;
    for (int i = 0; valid_rates[i]; i++)
        if (sample_rate == valid_rates[i]) { rate_ok = 1; break; }

    if (!rate_ok) {
        audio_log("[audio] configure: unsupported rate %d, falling back to 48000\n",
                  sample_rate);
        sample_rate = 48000;
    }

    channels = (channels < 1) ? 1 : (channels > 2) ? 2 : channels;
    if (bits != 16 && bits != 8) bits = 16;  /* Vita only does 16-bit out */

    ctx->sample_rate     = sample_rate;
    ctx->channels        = channels;
    ctx->bits            = bits;
    ctx->bytes_per_frame = channels * (bits / 8);

    /* Preroll: use the requested ms but floor at 4 grains of silence-free
       headroom.  This keeps us well above the underrun threshold. */
    int min_ms   = (AUDIO_GRAIN * 4 * 1000) / sample_rate + 1;
    if (preroll_ms < min_ms) preroll_ms = min_ms;
    ctx->preroll_bytes = (long long)sample_rate * channels * (bits / 8)
                         * preroll_ms / 1000;
    ctx->prerolled     = 0;

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

    audio_log("[audio] configure: rate=%d ch=%d bits=%d preroll=%dms "
              "preroll_bytes=%d grain=%d  port=%d\n",
              sample_rate, channels, bits, preroll_ms,
              ctx->preroll_bytes, AUDIO_GRAIN, port);

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
    int w = atomic_load_acq(&ctx->write_pos);
    atomic_store_rel(&ctx->read_pos, w);
    __atomic_store_n(&ctx->prerolled, 0, __ATOMIC_RELEASE);
    ctx->stat_underruns = 0;
    ctx->stat_overflows = 0;
}

void audio_start(AudioContext *ctx) {
    if (ctx->running || ctx->port < 0) return;
    __atomic_store_n(&ctx->running, 1, __ATOMIC_RELEASE);

    ctx->thread_id = sceKernelCreateThread(
        "audio_out", audio_thread,
        0x10000100,   /* realtime priority */
        0x10000,      /* 64 KB stack        */
        0, 0, NULL
    );
    if (ctx->thread_id >= 0) {
        AudioContext *arg = ctx;
        sceKernelStartThread(ctx->thread_id, sizeof(arg), &arg);
        audio_log("[audio] thread started\n");
    } else {
        audio_log("[audio] thread create failed: %d\n", ctx->thread_id);
        __atomic_store_n(&ctx->running, 0, __ATOMIC_RELEASE);
    }
}

void audio_stop(AudioContext *ctx) {
    if (!ctx->running) return;
    __atomic_store_n(&ctx->running, 0, __ATOMIC_RELEASE);
    if (ctx->thread_id >= 0) {
        sceKernelWaitThreadEnd(ctx->thread_id, NULL, NULL);
        sceKernelDeleteThread(ctx->thread_id);
        ctx->thread_id = -1;
    }
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
