#include "types.h"
#include "json.h"
#include "config.h"
#include "network.h"
#include "audio.h"
#include "gui.h"
#include "time_sync.h"
#include "diagnostics.h"
#include <opus/opus.h>

#include <math.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <psp2/sysmodule.h>
#include <psp2/net/netctl.h>
#include <psp2/rtc.h>
#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

#if SNAPVITA_FILE_LOGGING
static void main_log(const char *fmt, ...) {
    /* Ensure directory exists (Vita may not create it automatically) */
    sceIoMkdir("ux0:data", 0777);
    sceIoMkdir("ux0:data/snapcast", 0777);
    SceUID fd = sceIoOpen("ux0:data/snapcast/debug.log",
                          SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
    if (fd < 0) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (len > 0) sceIoWrite(fd, buf, len < sizeof(buf) ? len : sizeof(buf) - 1);
    sceIoClose(fd);
}
#else
#define main_log(...) ((void)0)
#endif

static AppState g_state;
static NetContext g_net;
static AudioContext g_audio;
static TimeSync g_time_sync;
static OpusDecoder *g_opus_dec = NULL;

static SyncFilter g_sync_filter;
static int g_hard_sync = 1;
static int64_t g_last_correction_usec = -1;
static int64_t g_last_sync_log_usec = -1;
static int64_t g_next_chunk_usec = -1;

#define SOFT_SYNC_BEGIN_USEC  100
#define HARD_SYNC_USEC       500000
#define TIMESTAMP_GAP_USEC   50000
#define CORRECTION_INTERVAL_USEC 100000

/* --- Streaming thread --- */

static int stream_thread_running = 0;
static SceUID stream_thread_id = -1;

static void fail_stream(const char *message) {
    snprintf(g_state.conn_error, sizeof(g_state.conn_error), "%s", message);
    g_state.conn_state = CONN_ERROR;
    __atomic_store_n(&stream_thread_running, 0, __ATOMIC_RELEASE);
}

static void check_audio_output(void) {
    if (g_state.conn_state == CONN_CONNECTED &&
        __atomic_load_n(&g_audio.output_error, __ATOMIC_ACQUIRE) < 0) {
        main_log("[audio] output failed: %d\n",
                 __atomic_load_n(&g_audio.output_error, __ATOMIC_ACQUIRE));
        fail_stream("Audio output failed");
    }
}

static void parse_server_settings(const char *payload, uint32_t size, AppState *state) {
    if (size < 4) return;
    uint32_t json_len;
    memcpy(&json_len, payload, sizeof(uint32_t));
    if (json_len > size - 4) return;

    char *json_str = (char *)malloc(json_len + 1);
    if (!json_str) return;
    memcpy(json_str, payload + 4, json_len);
    json_str[json_len] = '\0';

    JsonNode *root = json_parse(json_str);
    free(json_str);
    if (!root) return;

    state->buffer_ms      = json_get_int(root, "bufferMs", 1000);
    state->config.latency_ms = json_get_int(root, "latency", state->config.latency_ms);
    state->volume_percent = json_get_int(root, "volume",    100);
    state->volume_muted   = json_get_bool(root, "muted",     0);

    main_log("[main] server_settings: bufferMs=%d vol=%d muted=%d\n",
             state->buffer_ms, state->volume_percent, state->volume_muted);

    audio_set_volume(&g_audio, state->volume_percent, state->volume_muted);
    json_free(root);
}

static int parse_codec_header(const char *payload, uint32_t size, AppState *state) {
    if (size < 4) return -1;
    uint32_t codec_len;
    memcpy(&codec_len, payload, sizeof(uint32_t));
    if (!codec_len || codec_len > size - 4 || codec_len >= sizeof(state->codec)) return -1;

    /* Do not publish a new codec with the previous codec's sample format. */
    char codec[sizeof(state->codec)];
    memcpy(codec, payload + 4, codec_len);
    codec[codec_len] = '\0';
    if (memchr(codec, '\0', codec_len)) return -1;

    const char *p = payload + 4 + codec_len;
    uint32_t remaining = size - 4 - codec_len;

    if (remaining < 4) return -1;
    uint32_t header_size;
    memcpy(&header_size, p, sizeof(uint32_t));
    p += 4;
    remaining -= 4;

    if (header_size > remaining) return -1;

    /* Parse sample format from codec header */
    uint32_t sr;
    uint16_t ch, bits;
    if (strcmp(codec, "pcm") == 0 && header_size >= 44) {
        uint32_t fmt_size;
        uint16_t format;
        memcpy(&fmt_size, p + 16, sizeof(fmt_size));
        memcpy(&format, p + 20, sizeof(format));
        if (memcmp(p, "RIFF", 4) || memcmp(p + 8, "WAVEfmt ", 8) ||
            memcmp(p + 36, "data", 4) || fmt_size != 16 || format != 1)
            return -1;
        /* RIFF WAVE header: bytes 24-27 = sample rate, 34-35 = bits, 22-23 = channels */
        memcpy(&sr, p + 24, sizeof(uint32_t));
        memcpy(&ch, p + 22, sizeof(uint16_t));
        memcpy(&bits, p + 34, sizeof(uint16_t));
    } else if (strcmp(codec, "opus") == 0 && header_size >= 12) {
        uint32_t magic;
        memcpy(&magic, p, sizeof(magic));
        if (magic != 0x4F505553) return -1; /* Snapcast's custom Opus header */
        memcpy(&sr, p + 4, sizeof(uint32_t));
        memcpy(&bits, p + 8, sizeof(uint16_t));
        memcpy(&ch, p + 10, sizeof(uint16_t));
    } else return -1;
    if (sr == 0 || sr > INT_MAX || (ch != 1 && ch != 2) || bits != 16) return -1;
    memcpy(state->codec, codec, codec_len + 1);
    state->sample_rate = (int)sr;
    state->channels = ch;
    state->bits = bits;
    return 0;
}

/* -----------------------------------------------------------------------
 * PCM dump – saves the first DUMP_BYTES bytes of received audio to a file.
 * Transfer ux0:data/snapcast/audio_dump.raw to PC and play with:
 *   sox -r 48000 -e signed-integer -b 16 -c 2 -L audio_dump.raw dump.wav
 * If it sounds clean there, the distortion is in the Vita audio output path.
 * If it also sounds distorted, the issue is in data reception.
 * Enable only with the SNAPVITA_AUDIO_DUMP CMake option.
 * ----------------------------------------------------------------------- */
#if SNAPVITA_AUDIO_DUMP
#define DUMP_BYTES          (192000 * 5)   /* 5 seconds @ 48 kHz stereo 16 bit */
#define DUMP_FILE           "ux0:data/snapcast/audio_dump.raw"

static SceUID g_dump_fd    = -1;
static int    g_dump_bytes = 0;
static int    g_dump_done  = 0;

static void pcm_dump_init(void) {
    g_dump_done = 0;
    g_dump_bytes = 0;
    if (g_dump_fd >= 0) { sceIoClose(g_dump_fd); g_dump_fd = -1; }
    sceIoRemove(DUMP_FILE);
    g_dump_fd = sceIoOpen(DUMP_FILE,
                          SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    main_log("[dump] opened %s  fd=%d\n", DUMP_FILE, g_dump_fd);
}

static void pcm_dump_write(const void *data, int size) {
    if (g_dump_done || g_dump_fd < 0) return;
    int to_write = size;
    if (g_dump_bytes + to_write > DUMP_BYTES)
        to_write = DUMP_BYTES - g_dump_bytes;
    if (to_write > 0) {
        sceIoWrite(g_dump_fd, data, to_write);
        g_dump_bytes += to_write;
    }
    if (g_dump_bytes >= DUMP_BYTES) {
        sceIoClose(g_dump_fd);
        g_dump_fd   = -1;
        g_dump_done = 1;
        main_log("[dump] complete: %d bytes written to %s\n",
                 g_dump_bytes, DUMP_FILE);
    }
}
#else
#define pcm_dump_init() ((void)0)
#define pcm_dump_write(...) ((void)0)
#endif

/* Correct clock drift and output scheduling overhead without flushing audio.
 * Keep the original correction headroom; real source gaps reset this history. */
static void compute_soft_sync(int64_t now_usec, int64_t age_usec) {
    int correction = 0;
    if (sync_filter_ready(&g_sync_filter, now_usec)) {
        int64_t median = sync_filter_median(&g_sync_filter, now_usec);
        if ((median > SOFT_SYNC_BEGIN_USEC && age_usec > SOFT_SYNC_BEGIN_USEC) ||
            (median < -SOFT_SYNC_BEGIN_USEC && age_usec < -SOFT_SYNC_BEGIN_USEC)) {
            double rate = (double)median / 2000000.0;
            if (rate > 0.06) rate = 0.06;
            if (rate < -0.06) rate = -0.06;
            correction = (int)round(1.0 / rate);
        }
    }
    __atomic_store_n(&g_audio.correct_after_x_frames, correction, __ATOMIC_RELEASE);
}

static void reset_sync_buffers(void) {
    sync_filter_reset(&g_sync_filter);
    g_last_correction_usec = -1;
    g_last_sync_log_usec = -1;
    g_next_chunk_usec = -1;
    g_hard_sync = 1;
    g_state.in_hard_sync = 1;
    g_state.last_age_usec = 0;
    __atomic_store_n(&g_audio.correct_after_x_frames, 0, __ATOMIC_RELEASE);
}

static int restart_preroll(void) {
    audio_stop(&g_audio);
    audio_flush(&g_audio);
    reset_sync_buffers();
    if (audio_start(&g_audio) < 0) {
        fail_stream("Failed to restart audio thread");
        return -1;
    }
    return 0;
}

/* Queue the first playable samples at their server timestamp plus effective
 * delay. Old packets and elapsed prefixes are discarded without changing pitch. */
static void write_scheduled_chunk(const char *pcm_data, uint32_t chunk_size,
                                  int64_t chunk_ts_usec) {
    int64_t due = chunk_ts_usec + (int64_t)g_audio.buffer_ms * 1000
                  - g_time_sync.diff_usec;
    int64_t late = get_tick_usec() - due;
    if (late > 0) {
        int64_t frames = chunk_size / g_audio.bytes_per_frame;
        if (late >= frames * 1000000 / g_audio.sample_rate) return;
        int64_t skip = (late * g_audio.sample_rate + 999999) / 1000000;
        if (skip >= frames) return;
        pcm_data += skip * g_audio.bytes_per_frame;
        chunk_size -= skip * g_audio.bytes_per_frame;
        due += skip * 1000000 / g_audio.sample_rate;
    }
    if (audio_write(&g_audio, pcm_data, chunk_size) == (int)chunk_size)
        audio_schedule(&g_audio, due);
}

static void handle_wire_chunk(const char *payload, uint32_t size) {
    if (size < 12) return;

    /* Wire chunk body: timestamp.sec(4) + timestamp.usec(4) + size(4) + data */
    int32_t ts_sec, ts_usec;
    uint32_t chunk_size;
    memcpy(&ts_sec,     payload,     sizeof(int32_t));
    memcpy(&ts_usec,    payload + 4, sizeof(int32_t));
    memcpy(&chunk_size, payload + 8, sizeof(uint32_t));
    if (chunk_size > size - 12) return;

    const char *pcm_data = payload + 12;

    /* Measure compressed wire bitrate (bytes before any decode) */
    {
        static int64_t bw_last_usec = 0;
        static int64_t bw_bytes     = 0;
        int64_t now = get_tick_usec();
        bw_bytes += chunk_size;
        if (bw_last_usec == 0) bw_last_usec = now;
        int64_t elapsed = now - bw_last_usec;
        if (elapsed >= 1000000) {
            __atomic_store_n(&g_state.wire_kbps,
                             (int)(bw_bytes * 8000LL / elapsed),
                             __ATOMIC_RELEASE);
            bw_bytes     = 0;
            bw_last_usec = now;
        }
    }

    /* Decode Opus frames into PCM before any further processing */
    static opus_int16 s_opus_buf[5760 * 2]; /* max 120 ms frame @ 48 kHz stereo */
    if (strcmp(g_state.codec, "opus") == 0) {
        if (!g_opus_dec) return;
        int frames = opus_decode(g_opus_dec,
                                 (const unsigned char *)pcm_data, (opus_int32)chunk_size,
                                 s_opus_buf, 5760, 0);
        if (frames <= 0) {
            main_log("[opus] decode error: %d (%s)\n", frames, opus_strerror(frames));
            return;
        }
        pcm_data  = (const char *)s_opus_buf;
        chunk_size = (uint32_t)(frames * g_audio.channels * sizeof(opus_int16));
    }

    static int first_chunk_logged = 0;
    if (!first_chunk_logged) {
        main_log("[main] first wire_chunk: chunk_size=%u (expected 3840 @ 48kHz/20ms)\n",
                 chunk_size);
        first_chunk_logged = 1;
    }

    pcm_dump_write(pcm_data, (int)chunk_size);

    /* Validate decoded PCM before touching the ring, including frame alignment. */
    int bpf = g_audio.bytes_per_frame;
    if (bpf <= 0 || chunk_size == 0 || chunk_size > AUDIO_MAX_CHUNK_SIZE ||
        chunk_size % bpf != 0) {
        main_log("[sync] invalid PCM chunk: %u bytes, frame=%d\n", chunk_size, bpf);
        return;
    }
    /* A timestamp cannot be scheduled until the server clock is known. */
    if (!time_sync_valid(&g_time_sync)) return;

    int effective_ms = audio_buffer_ms(&g_audio,
                           (int64_t)g_state.buffer_ms - g_state.config.latency_ms);
    if (effective_ms != g_audio.buffer_ms) {
        main_log("[sync] delay changed: %d -> %d ms\n", g_audio.buffer_ms, effective_ms);
        g_audio.buffer_ms = effective_ms;
        if (restart_preroll() < 0) return;
    }
    if (__atomic_load_n(&g_audio.needs_resync, __ATOMIC_ACQUIRE)) {
        main_log("[sync] rebuffer after underrun\n");
        if (restart_preroll() < 0) return;
    }

    int64_t chunk_ts_usec = (int64_t)ts_sec * 1000000 + ts_usec;
    int64_t discontinuity = g_next_chunk_usec < 0 ? 0 : chunk_ts_usec - g_next_chunk_usec;
    if (discontinuity > TIMESTAMP_GAP_USEC || discontinuity < -TIMESTAMP_GAP_USEC) {
        main_log("[sync] timestamp discontinuity: %lld us\n", (long long)discontinuity);
        if (restart_preroll() < 0) return;
    }
    g_next_chunk_usec = chunk_ts_usec +
                       (int64_t)(chunk_size / bpf) * 1000000 / g_audio.sample_rate;

    if (g_hard_sync && __atomic_load_n(&g_audio.prerolled, __ATOMIC_ACQUIRE)) {
        g_hard_sync = 0;
        g_state.in_hard_sync = 0;
        main_log("[sync] scheduled playback started, effective buffer=%d ms\n",
                 g_audio.buffer_ms);
    }
    if (!__atomic_load_n(&g_audio.scheduled, __ATOMIC_ACQUIRE)) {
        write_scheduled_chunk(pcm_data, chunk_size, chunk_ts_usec);
        return;
    }

    int64_t now = get_tick_usec();
    int64_t age_usec = 0;
    if (!g_hard_sync) {
        int64_t ring_usec = (int64_t)audio_available(&g_audio) * 1000000 /
                           ((int64_t)g_audio.sample_rate * bpf);
        age_usec = now + g_time_sync.diff_usec - chunk_ts_usec -
                   (int64_t)g_audio.buffer_ms * 1000 + ring_usec;
        g_state.last_age_usec = age_usec;
        /* Allow soft correction to catch up before discarding a healthy queue.
         * Smaller buffers need earlier recovery; allow at least two grains
         * because queue sampling is quantized, especially at low rates. */
        int64_t hard_limit = (int64_t)g_audio.buffer_ms * 500;
        if (hard_limit > HARD_SYNC_USEC) hard_limit = HARD_SYNC_USEC;
        int64_t grain_limit = (int64_t)AUDIO_GRAIN * 2000000 / g_audio.sample_rate;
        if (hard_limit < grain_limit) hard_limit = grain_limit;
        if (age_usec >= hard_limit || age_usec <= -hard_limit) {
            main_log("[sync] rebuffer: age=%lld us ring=%lld us\n",
                     (long long)age_usec, (long long)ring_usec);
            if (restart_preroll() < 0) return;
            write_scheduled_chunk(pcm_data, chunk_size, chunk_ts_usec);
            return;
        }
    }

    if (audio_write(&g_audio, pcm_data, chunk_size) != (int)chunk_size) {
        /* Stop the consumer before discarding any queued data. */
        main_log("[sync] ring full: rescheduling current chunk\n");
        if (restart_preroll() < 0) return;
        write_scheduled_chunk(pcm_data, chunk_size, chunk_ts_usec);
        return;
    }
    if (g_hard_sync) return;

    sync_filter_add(&g_sync_filter, now, age_usec);
    if (g_last_correction_usec < 0 || now - g_last_correction_usec >= CORRECTION_INTERVAL_USEC) {
        compute_soft_sync(now, age_usec);
        g_last_correction_usec = now;
    }
    if (g_last_sync_log_usec < 0 || now - g_last_sync_log_usec >= 1000000) {
        g_last_sync_log_usec = now;
        main_log("[sync] age=%lld median=%lld corr=%d effective=%dms\n",
                 (long long)age_usec, (long long)sync_filter_median(&g_sync_filter, now),
                 __atomic_load_n(&g_audio.correct_after_x_frames, __ATOMIC_ACQUIRE),
                 g_audio.buffer_ms);
    }
}

static int stream_thread_func(SceSize args, void *argp) {
    AppState *state = &g_state;
    NetContext *net = &g_net;

    main_log("[main] connecting to %s:%d\n",
             state->config.server_ip, state->config.stream_port);

    if (net_stream_connect(net, state->config.server_ip, state->config.stream_port) < 0) {
        main_log("[main] stream connect failed\n");
        strncpy(state->conn_error, "Failed to connect (stream)", MAX_STR_LEN - 1);
        state->conn_state = CONN_ERROR;
        __atomic_store_n(&stream_thread_running, 0, __ATOMIC_RELEASE);
        return 0;
    }

    if (net_send_hello(net, &state->config) < 0) {
        strncpy(state->conn_error, "Failed to send hello", MAX_STR_LEN - 1);
        state->conn_state = CONN_ERROR;
        net_stream_disconnect(net);
        __atomic_store_n(&stream_thread_running, 0, __ATOMIC_RELEASE);
        return 0;
    }

    int codec_received = 0;
    int64_t last_time_request_usec = -1;

    while (__atomic_load_n(&stream_thread_running, __ATOMIC_ACQUIRE)) {
        BaseMessage hdr;
        char *payload = NULL;

        if (net_recv_message(net, &hdr, &payload) < 0) {
            if (__atomic_load_n(&stream_thread_running, __ATOMIC_ACQUIRE)) {
                strncpy(state->conn_error, "Connection lost", MAX_STR_LEN - 1);
                state->conn_state = CONN_ERROR;
            }
            break;
        }

        {
            static int first_msg = 1;
            if (first_msg) {
                first_msg = 0;
                main_log("[main] first stream msg: type=%u size=%u\n",
                         (unsigned)hdr.type, (unsigned)hdr.size);
            }
        }

        switch (hdr.type) {
            case MSG_SERVER_SETTINGS:
                parse_server_settings(payload, hdr.size, state);
                break;

            case MSG_CODEC_HEADER:
                if (parse_codec_header(payload, hdr.size, state) < 0) {
                    codec_received = 0;
                    fail_stream("Invalid or unsupported codec header");
                    break;
                }
                codec_received = 1;
                {
                    int64_t requested_buf = (int64_t)state->buffer_ms - state->config.latency_ms;
                    int eff_buf = requested_buf < 50 ? 50 : requested_buf > 5000 ? 5000 : (int)requested_buf;
                    main_log("[main] codec_header: codec='%s' rate=%d ch=%d bits=%d"
                             " bufMs=%d latMs=%d preroll=%dms\n",
                             state->codec, state->sample_rate, state->channels,
                             state->bits, state->buffer_ms,
                             state->config.latency_ms, eff_buf);

                    /* (Re-)create Opus decoder when the server uses Opus */
                    if (g_opus_dec) {
                        opus_decoder_destroy(g_opus_dec);
                        g_opus_dec = NULL;
                    }
                    if (strcmp(state->codec, "opus") == 0) {
                        int err = 0;
                        int dec_rate = state->sample_rate;
                        int dec_ch   = state->channels;
                        g_opus_dec = opus_decoder_create(dec_rate, dec_ch, &err);
                        if (g_opus_dec)
                            main_log("[opus] decoder created: rate=%d ch=%d\n",
                                     dec_rate, dec_ch);
                        else
                            main_log("[opus] decoder create failed: err=%d\n", err);
                    }

                    pcm_dump_init();
                    audio_stop(&g_audio);
                    audio_flush(&g_audio);
                    reset_sync_buffers();
                    int audio_result = audio_configure(&g_audio,
                        state->sample_rate, state->channels, state->bits,
                        eff_buf);
                    if (audio_result < 0 ||
                        (strcmp(state->codec, "pcm") != 0 && strcmp(state->codec, "opus") != 0) ||
                        (strcmp(state->codec, "opus") == 0 && !g_opus_dec)) {
                        codec_received = 0;
                        fail_stream("Unsupported audio format or audio output unavailable");
                    } else if (audio_start(&g_audio) < 0) {
                        codec_received = 0;
                        fail_stream("Failed to start audio thread");
                    }
                }
                break;

            case MSG_WIRE_CHUNK:
                if (codec_received)
                    handle_wire_chunk(payload, hdr.size);
                break;

            case MSG_TIME:
                if (hdr.size >= 8) {
                    int32_t lat_sec, lat_usec;
                    memcpy(&lat_sec,  payload,     sizeof(int32_t));
                    memcpy(&lat_usec, payload + 4, sizeof(int32_t));
                    int64_t c2s = (int64_t)lat_sec * 1000000 + lat_usec;
                    int64_t s2c = ((int64_t)hdr.received_sec - hdr.sent_sec) * 1000000
                                + (hdr.received_usec - hdr.sent_usec);
                    time_sync_update(&g_time_sync, c2s, s2c);

                    /* Keep AppState in sync for GUI display */
                    g_state.time_diff_usec  = g_time_sync.diff_usec;
                    g_state.time_sync_count = g_time_sync.count;

                    static int ts_log_count = 0;
                    ts_log_count++;
                    if (ts_log_count <= 10 || ts_log_count % 50 == 0) {
                        main_log("[time] #%d c2s=%lld s2c=%lld diff=%lld us\n",
                                 ts_log_count,
                                 (long long)c2s, (long long)s2c,
                                 (long long)g_time_sync.diff_usec);
                    }
                }
                break;

            case MSG_ERROR:
                if (hdr.size >= 8) {
                    uint32_t code, err_len;
                    memcpy(&code, payload, 4);
                    memcpy(&err_len, payload + 4, 4);
                    if (err_len < MAX_STR_LEN && err_len <= hdr.size - 8) {
                        memcpy(state->conn_error, payload + 8, err_len);
                        state->conn_error[err_len] = '\0';
                    }
                }
                break;

            default:
                break;
        }

        free(payload);
        if (!__atomic_load_n(&stream_thread_running, __ATOMIC_ACQUIRE)) break;

        /* Fast initial convergence, then elapsed-time cadence independent of
         * packet duration or bursts of queued chunks. Idle sockets sync on wake. */
        int64_t now_usec = get_tick_usec();
        if (g_time_sync.count < 50 || last_time_request_usec < 0 ||
            now_usec - last_time_request_usec >= 1000000) {
            net_send_time(net);
            last_time_request_usec = now_usec;
        }
    }

    net_stream_disconnect(net);
    __atomic_store_n(&stream_thread_running, 0, __ATOMIC_RELEASE);
    return 0;
}

static void start_streaming(void) {
    if (__atomic_load_n(&stream_thread_running, __ATOMIC_ACQUIRE)) return;
    __atomic_store_n(&stream_thread_running, 1, __ATOMIC_RELEASE);

    stream_thread_id = sceKernelCreateThread("stream_thread", stream_thread_func,
        0x10000100, 0x10000, 0, 0, NULL);
    if (stream_thread_id >= 0) {
        int result = sceKernelStartThread(stream_thread_id, 0, NULL);
        if (result < 0) {
            main_log("[main] stream thread start failed: %d\n", result);
            sceKernelDeleteThread(stream_thread_id);
            stream_thread_id = -1;
            fail_stream("Failed to start stream thread");
        }
    } else {
        main_log("[main] stream thread create failed: %d\n", stream_thread_id);
        stream_thread_id = -1;
        fail_stream("Failed to create stream thread");
    }
}

static void stop_streaming(void) {
    __atomic_store_n(&stream_thread_running, 0, __ATOMIC_RELEASE);
    net_stream_disconnect(&g_net);
    if (stream_thread_id >= 0) {
        sceKernelWaitThreadEnd(stream_thread_id, NULL, NULL);
        sceKernelDeleteThread(stream_thread_id);
        stream_thread_id = -1;
    }
    audio_stop(&g_audio);
    if (g_opus_dec) {
        opus_decoder_destroy(g_opus_dec);
        g_opus_dec = NULL;
    }
}

/* --- JSON-RPC message processing --- */

static void process_rpc_message(const char *json_str) {
    JsonNode *root = json_parse(json_str);
    if (!root) return;

    const char *method = json_get_string(root, "method", NULL);
    JsonNode *result = json_get(root, "result");
    JsonNode *params = json_get(root, "params");

    if (result) {
        /* Response to our request */
        JsonNode *server = json_get(result, "server");
        if (server) {
            net_parse_server_status(result, &g_state.server);
        }
    }

    if (method) {
        if (strcmp(method, "Server.OnUpdate") == 0 && params) {
            net_parse_server_status(params, &g_state.server);
        }
        else if (strcmp(method, "Client.OnVolumeChanged") == 0 && params) {
            const char *id = json_get_string(params, "id", "");
            JsonNode *vol = json_get(params, "volume");
            if (vol && strcmp(id, g_state.our_client_id) == 0) {
                g_state.volume_percent = json_get_int(vol, "percent", g_state.volume_percent);
                g_state.volume_muted = json_get_bool(vol, "muted", g_state.volume_muted);
                audio_set_volume(&g_audio, g_state.volume_percent, g_state.volume_muted);
            }
            /* Update in the server state too */
            for (int g = 0; g < g_state.server.group_count; g++)
                for (int c = 0; c < g_state.server.groups[g].client_count; c++)
                    if (strcmp(g_state.server.groups[g].clients[c].id, id) == 0 && vol) {
                        g_state.server.groups[g].clients[c].volume.percent =
                            json_get_int(vol, "percent", 100);
                        g_state.server.groups[g].clients[c].volume.muted =
                            json_get_bool(vol, "muted", 0);
                    }
        }
        else if (strcmp(method, "Client.OnLatencyChanged") == 0 && params) {
            const char *id = json_get_string(params, "id", "");
            if (strcmp(id, g_state.our_client_id) == 0)
                g_state.config.latency_ms = json_get_int(params, "latency", 0);
        }
        else if (strcmp(method, "Client.OnNameChanged") == 0 && params) {
            const char *id = json_get_string(params, "id", "");
            const char *name = json_get_string(params, "name", "");
            if (strcmp(id, g_state.our_client_id) == 0)
                strncpy(g_state.config.client_name, name, MAX_NAME_LEN - 1);
        }
        else if (strcmp(method, "Stream.OnProperties") == 0 && params) {
            const char *id = json_get_string(params, "id", "");
            JsonNode *props = json_get(params, "properties");
            if (props) {
                for (int s = 0; s < g_state.server.stream_count; s++) {
                    if (strcmp(g_state.server.streams[s].id, id) == 0)
                        net_parse_stream_properties(props, &g_state.server.streams[s]);
                }
            }
        }
        else if (strcmp(method, "Stream.OnUpdate") == 0 && params) {
            /* Refresh full server state */
            net_rpc_get_status(&g_net);
        }
        else if (strcmp(method, "Group.OnMute") == 0 && params) {
            const char *id = json_get_string(params, "id", "");
            int mute = json_get_bool(params, "mute", 0);
            for (int g = 0; g < g_state.server.group_count; g++)
                if (strcmp(g_state.server.groups[g].id, id) == 0)
                    g_state.server.groups[g].muted = mute;
        }
        else if (strcmp(method, "Group.OnStreamChanged") == 0 && params) {
            const char *id = json_get_string(params, "id", "");
            const char *sid = json_get_string(params, "stream_id", "");
            for (int g = 0; g < g_state.server.group_count; g++)
                if (strcmp(g_state.server.groups[g].id, id) == 0)
                    strncpy(g_state.server.groups[g].stream_id, sid, MAX_ID_LEN - 1);
        }
        else if (strcmp(method, "Client.OnConnect") == 0 ||
                 strcmp(method, "Client.OnDisconnect") == 0) {
            net_rpc_get_status(&g_net);
        }
    }

    json_free(root);
}

/* --- Try to identify our own client ID from server state --- */

static void identify_self(AppState *state) {
    if (state->our_client_id[0]) return;

    SceNetCtlInfo info;
    char our_ip[MAX_IP_LEN] = "";
    if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &info) >= 0)
        strncpy(our_ip, info.ip_address, MAX_IP_LEN - 1);

    for (int g = 0; g < state->server.group_count; g++) {
        for (int c = 0; c < state->server.groups[g].client_count; c++) {
            SnapClient *cli = &state->server.groups[g].clients[c];
            if (strcmp(cli->ip, our_ip) == 0 && cli->connected) {
                strncpy(state->our_client_id, cli->id, MAX_ID_LEN - 1);
                state->volume_percent = cli->volume.percent;
                state->volume_muted = cli->volume.muted;
                return;
            }
        }
    }
}

/* --- Network profile helpers --- */

/* Extract the /24 prefix (first 3 octets) of the Vita's current IP into out. */
static int get_net_prefix(char *out, size_t sz) {
    SceNetCtlInfo info;
    if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &info) < 0)
        return -1;
    const char *last_dot = strrchr(info.ip_address, '.');
    if (!last_dot) return -1;
    int prefix_len = (int)(last_dot - info.ip_address);
    if ((size_t)prefix_len >= sz) return -1;
    memcpy(out, info.ip_address, prefix_len);
    out[prefix_len] = '\0';
    return 0;
}

/* If a saved profile exists for the current /24 network, copy its server IP
 * into cfg->server_ip so the correct default is shown and used on startup. */
static void apply_network_profile(AppConfig *cfg) {
    char prefix[MAX_IP_LEN] = "";
    if (get_net_prefix(prefix, sizeof(prefix)) < 0) return;
    for (int i = 0; i < cfg->net_profile_count; i++) {
        if (strcmp(cfg->net_profiles[i].net_prefix, prefix) == 0) {
            strncpy(cfg->server_ip, cfg->net_profiles[i].server_ip,
                    MAX_IP_LEN - 1);
            main_log("[config] network profile match for %s -> %s\n",
                     prefix, cfg->server_ip);
            return;
        }
    }
}

/* Save (or update) the server IP used for the current /24 network. */
static void update_network_profile(AppConfig *cfg) {
    char prefix[MAX_IP_LEN] = "";
    if (get_net_prefix(prefix, sizeof(prefix)) < 0) return;
    for (int i = 0; i < cfg->net_profile_count; i++) {
        if (strcmp(cfg->net_profiles[i].net_prefix, prefix) == 0) {
            snprintf(cfg->net_profiles[i].server_ip, MAX_IP_LEN,
                     "%s", cfg->server_ip);
            main_log("[config] network profile updated for %s: %s\n",
                     prefix, cfg->server_ip);
            return;
        }
    }
    if (cfg->net_profile_count < MAX_NET_PROFILES) {
        snprintf(cfg->net_profiles[cfg->net_profile_count].net_prefix,
                 MAX_IP_LEN, "%s", prefix);
        snprintf(cfg->net_profiles[cfg->net_profile_count].server_ip,
                 MAX_IP_LEN, "%s", cfg->server_ip);
        cfg->net_profile_count++;
        main_log("[config] network profile added for %s: %s\n",
                 prefix, cfg->server_ip);
    }
}

/* --- Connection management --- */

static void cleanup_connection(NetContext *net) {
    stop_streaming();
    net_control_disconnect(net);
}

static void do_connect(AppState *state, NetContext *net) {
    cleanup_connection(net);
    memset(&g_state.server, 0, sizeof(g_state.server));
    g_state.our_client_id[0] = '\0';
    g_state.codec[0] = '\0';
    g_state.sample_rate = 0;
    g_state.bits = 0;
    g_state.channels = 0;
    g_state.time_diff_usec  = 0;
    g_state.time_sync_count = 0;
    g_state.last_age_usec   = 0;
    g_state.in_hard_sync    = 1;
    g_state.wire_kbps       = 0;
    g_state.conn_error[0]   = '\0';
    __atomic_store_n(&g_audio.output_error, 0, __ATOMIC_RELEASE);
    audio_flush(&g_audio);
    time_sync_init(&g_time_sync);
    reset_sync_buffers();

    if (net_control_connect(net, state->config.server_ip, state->config.control_port) < 0) {
        strncpy(state->conn_error, "Failed to connect (control)", MAX_STR_LEN - 1);
        state->conn_state = CONN_ERROR;
        return;
    }

    state->conn_state = CONN_CONNECTED;
    state->auto_reconnect = 1;
    start_streaming();
    net_rpc_get_status(net);
}

static void do_disconnect(AppState *state, NetContext *net) {
    stop_streaming();
    net_control_disconnect(net);
    state->conn_state = CONN_DISCONNECTED;
    state->auto_reconnect = 0;
    state->reconnect_attempts = 0;
}

/* --- Entry point --- */

int main(void) {
    /* Establish the relative tick base before any timing code runs.
     * This prevents int32_t overflow in get_time() / net_send_time(). */
    time_sync_tick_init();

    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

    sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    sceSysmoduleLoadModule(SCE_SYSMODULE_IME);

    main_log("[main] snapcast-vita start\n");

    memset(&g_state, 0, sizeof(g_state));
    memset(&g_net, 0, sizeof(g_net));
    g_net.stream_sock = -1;
    g_net.control_sock = -1;
    g_state.running = 1;
    g_state.volume_percent = 100;

    int config_existed = config_load(&g_state.config);

    if (net_init() < 0) {
        strncpy(g_state.conn_error, "Network init failed", MAX_STR_LEN - 1);
        g_state.conn_state = CONN_ERROR;
    } else {
        /* Apply saved /24 network profile if one matches the current network.
         * This overrides the generic server_ip with the network-specific one. */
        apply_network_profile(&g_state.config);

        if (config_existed < 0) {
            /* No config file yet: profile list is empty, so fall back to
             * guessing .100 on the current /24 as the initial server IP. */
            SceNetCtlInfo info;
            if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &info) >= 0) {
                char *last_dot = strrchr(info.ip_address, '.');
                if (last_dot) {
                    int prefix_len = (int)(last_dot - info.ip_address);
                    snprintf(g_state.config.server_ip, MAX_IP_LEN,
                             "%.*s.100", prefix_len, info.ip_address);
                }
            }
        }
    }

    if (audio_init(&g_audio) < 0) {
        strncpy(g_state.conn_error, "Audio init failed", MAX_STR_LEN - 1);
    }

    time_sync_init(&g_time_sync);
    sync_filter_reset(&g_sync_filter);

    if (gui_init() < 0) {
        sceKernelExitProcess(0);
        return 1;
    }

    int rpc_poll_counter = 0;

    while (g_state.running) {
        /* Handle IME dialog */
        if (gui_ime_active()) {
            gui_ime_update(&g_state, &g_net);
        }

        /* Process input */
        gui_handle_input(&g_state, &g_net, &g_audio);

        if (g_state.conn_state == CONN_DISCONNECTED && stream_thread_id >= 0)
            cleanup_connection(&g_net);

        /* Handle connection state transitions */
        if (g_state.conn_state == CONN_CONNECTING) {
            do_connect(&g_state, &g_net);
            if (g_state.conn_state == CONN_CONNECTED) {
                update_network_profile(&g_state.config);
                config_save(&g_state.config);
            }
        }

        /* Poll JSON-RPC */
        if (g_state.conn_state == CONN_CONNECTED) {
            char *line = NULL;
            for (int i = 0; i < 10; i++) {
                if (net_rpc_poll(&g_net, &line) && line) {
                    process_rpc_message(line);
                    free(line);
                    line = NULL;
                } else {
                    break;
                }
            }

            /* Periodically re-fetch status and try to identify ourselves */
            rpc_poll_counter++;
            if (rpc_poll_counter >= 300) {
                net_rpc_get_status(&g_net);
                rpc_poll_counter = 0;
            }

            identify_self(&g_state);
        }

        check_audio_output();

        /* Check for stream thread failure */
        if (g_state.conn_state == CONN_CONNECTED && !__atomic_load_n(&stream_thread_running, __ATOMIC_ACQUIRE) &&
            stream_thread_id < 0 && g_state.conn_error[0]) {
            g_state.conn_state = CONN_ERROR;
        }

        /* Auto-reconnect: transition from error to reconnecting with backoff */
        if (g_state.conn_state == CONN_ERROR && g_state.auto_reconnect) {
            cleanup_connection(&g_net);
            int delay_secs = 1 << g_state.reconnect_attempts;
            if (delay_secs > 30) delay_secs = 30;
            g_state.reconnect_delay_frames = delay_secs * 60;
            g_state.reconnect_timer = 0;
            if (g_state.reconnect_attempts < 10)
                g_state.reconnect_attempts++;
            g_state.conn_state = CONN_RECONNECTING;
            main_log("[main] reconnecting in %ds (attempt %d)\n",
                     delay_secs, g_state.reconnect_attempts);
        }

        /* Handle reconnecting: wait for backoff delay then retry */
        if (g_state.conn_state == CONN_RECONNECTING) {
            g_state.reconnect_timer++;
            if (g_state.reconnect_timer >= g_state.reconnect_delay_frames) {
                main_log("[main] attempting reconnect (attempt %d)\n",
                         g_state.reconnect_attempts);
                do_connect(&g_state, &g_net);
                if (g_state.conn_state == CONN_CONNECTED) {
                    g_state.reconnect_attempts = 0;
                    update_network_profile(&g_state.config);
                    config_save(&g_state.config);
                    main_log("[main] reconnected successfully\n");
                }
            }
        }

        /* Reset the idle/suspend timer so the Vita never auto-suspends */
        sceKernelPowerTick(0);

        /* Render */
        gui_draw(&g_state, &g_audio);

        /* Check for exit */
        SceCtrlData ctrl;
        sceCtrlPeekBufferPositive(0, &ctrl, 1);
        if ((ctrl.buttons & SCE_CTRL_SELECT) && (ctrl.buttons & SCE_CTRL_START)) {
            g_state.running = 0;
        }
    }

    /* Cleanup */
    config_save(&g_state.config);
    do_disconnect(&g_state, &g_net);
    audio_fini(&g_audio);
    gui_fini();
    net_fini();

    sceKernelExitProcess(0);
    return 0;
}
