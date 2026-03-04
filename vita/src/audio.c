#include "audio.h"
#include <string.h>
#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>

#define AUDIO_VOLUME_MAX SCE_AUDIO_OUT_MAX_VOL

static int ring_readable(AudioContext *ctx) {
    int w = ctx->write_pos;
    int r = ctx->read_pos;
    if (w >= r) return w - r;
    return AUDIO_RING_SIZE - r + w;
}

static int ring_writable(AudioContext *ctx) {
    return AUDIO_RING_SIZE - ring_readable(ctx) - 1;
}

static int ring_read(AudioContext *ctx, void *buf, int size) {
    int avail = ring_readable(ctx);
    if (size > avail) size = avail;
    if (size <= 0) return 0;

    int r = ctx->read_pos;
    if (r + size <= AUDIO_RING_SIZE) {
        memcpy(buf, ctx->ring + r, size);
    } else {
        int first = AUDIO_RING_SIZE - r;
        memcpy(buf, ctx->ring + r, first);
        memcpy((uint8_t *)buf + first, ctx->ring, size - first);
    }
    ctx->read_pos = (r + size) % AUDIO_RING_SIZE;
    return size;
}

int audio_write(AudioContext *ctx, const void *data, int size) {
    int space = ring_writable(ctx);
    if (size > space) size = space;
    if (size <= 0) return 0;

    int w = ctx->write_pos;
    if (w + size <= AUDIO_RING_SIZE) {
        memcpy(ctx->ring + w, data, size);
    } else {
        int first = AUDIO_RING_SIZE - w;
        memcpy(ctx->ring + w, data, first);
        memcpy(ctx->ring, (const uint8_t *)data + first, size - first);
    }
    ctx->write_pos = (w + size) % AUDIO_RING_SIZE;
    return size;
}

int audio_available(AudioContext *ctx) {
    return ring_readable(ctx);
}

static int audio_thread(SceSize args, void *argp) {
    AudioContext *ctx = *(AudioContext **)argp;
    int16_t buf[AUDIO_GRAIN * AUDIO_CHANNELS];
    int frame_bytes = AUDIO_GRAIN * AUDIO_CHANNELS * sizeof(int16_t);

    while (ctx->running) {
        int got = ring_read(ctx, buf, frame_bytes);
        if (got < frame_bytes) {
            memset((uint8_t *)buf + got, 0, frame_bytes - got);
        }
        sceAudioOutOutput(ctx->port, buf);
    }
    return 0;
}

int audio_init(AudioContext *ctx) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->port = -1;
    ctx->volume_left = AUDIO_VOLUME_MAX;
    ctx->volume_right = AUDIO_VOLUME_MAX;

    ctx->port = sceAudioOutOpenPort(
        SCE_AUDIO_OUT_PORT_TYPE_MAIN,
        AUDIO_GRAIN,
        AUDIO_SAMPLE_RATE,
        SCE_AUDIO_OUT_MODE_STEREO
    );
    if (ctx->port < 0) return -1;

    int vol[2] = { AUDIO_VOLUME_MAX, AUDIO_VOLUME_MAX };
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

void audio_start(AudioContext *ctx) {
    if (ctx->running) return;
    ctx->running = 1;
    ctx->write_pos = 0;
    ctx->read_pos = 0;

    ctx->thread_id = sceKernelCreateThread("audio_thread", audio_thread,
        0x10000100, 0x4000, 0, 0, NULL);
    if (ctx->thread_id >= 0) {
        AudioContext *arg = ctx;
        sceKernelStartThread(ctx->thread_id, sizeof(arg), &arg);
    }
}

void audio_stop(AudioContext *ctx) {
    if (!ctx->running) return;
    ctx->running = 0;
    if (ctx->thread_id >= 0) {
        sceKernelWaitThreadEnd(ctx->thread_id, NULL, NULL);
        sceKernelDeleteThread(ctx->thread_id);
        ctx->thread_id = -1;
    }
}

void audio_set_volume(AudioContext *ctx, int percent, int muted) {
    ctx->muted = muted;
    if (ctx->port < 0) return;

    int v = muted ? 0 : (AUDIO_VOLUME_MAX * percent / 100);
    int vol[2] = { v, v };
    sceAudioOutSetVolume(ctx->port,
        SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vol);
    ctx->volume_left = v;
    ctx->volume_right = v;
}
