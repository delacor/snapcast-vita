#include "types.h"
#include "json.h"
#include "config.h"
#include "network.h"
#include "audio.h"
#include "gui.h"
#include "time_sync.h"

#include <math.h>
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
    sceIoWrite(fd, buf, len);
    sceIoClose(fd);
}

static AppState g_state;
static NetContext g_net;
static AudioContext g_audio;
static TimeSync g_time_sync;

/* Age tracking buffers for soft sync */
static AgeBuffer g_age_mini;
static AgeBuffer g_age_short;
static AgeBuffer g_age_long;
static int64_t   g_median_long  = 0;
static int64_t   g_median_short = 0;
static int       g_hard_sync    = 1;

/* Threshold below which soft sync kicks in (microseconds) */
#define SOFT_SYNC_BEGIN_USEC  100

/* --- Streaming thread --- */

static int stream_thread_running = 0;
static SceUID stream_thread_id = -1;

static void parse_server_settings(const char *payload, uint32_t size, AppState *state) {
    if (size < 4) return;
    uint32_t json_len;
    memcpy(&json_len, payload, sizeof(uint32_t));
    if (json_len > size - 4) return;

    char *json_str = (char *)malloc(json_len + 1);
    memcpy(json_str, payload + 4, json_len);
    json_str[json_len] = '\0';

    JsonNode *root = json_parse(json_str);
    free(json_str);
    if (!root) return;

    state->buffer_ms      = json_get_int(root, "bufferMs", 1000);
    state->volume_percent = json_get_int(root, "volume",    100);
    state->volume_muted   = json_get_bool(root, "muted",     0);

    main_log("[main] server_settings: bufferMs=%d vol=%d muted=%d\n",
             state->buffer_ms, state->volume_percent, state->volume_muted);

    audio_set_volume(&g_audio, state->volume_percent, state->volume_muted);
    json_free(root);
}

static void parse_codec_header(const char *payload, uint32_t size, AppState *state) {
    if (size < 4) return;
    uint32_t codec_len;
    memcpy(&codec_len, payload, sizeof(uint32_t));
    if (codec_len > size - 4 || codec_len >= sizeof(state->codec)) return;

    memcpy(state->codec, payload + 4, codec_len);
    state->codec[codec_len] = '\0';

    const char *p = payload + 4 + codec_len;
    uint32_t remaining = size - 4 - codec_len;

    if (remaining < 4) return;
    uint32_t header_size;
    memcpy(&header_size, p, sizeof(uint32_t));
    p += 4;
    remaining -= 4;

    if (header_size > remaining) return;

    /* Parse sample format from codec header */
    if (strcmp(state->codec, "pcm") == 0 && header_size >= 44) {
        /* RIFF WAVE header: bytes 24-27 = sample rate, 34-35 = bits, 22-23 = channels */
        uint32_t sr;
        uint16_t ch, bits;
        memcpy(&sr, p + 24, sizeof(uint32_t));
        memcpy(&ch, p + 22, sizeof(uint16_t));
        memcpy(&bits, p + 34, sizeof(uint16_t));
        state->sample_rate = sr;
        state->channels = ch;
        state->bits = bits;
    } else if (strcmp(state->codec, "opus") == 0 && header_size >= 12) {
        uint32_t sr;
        uint16_t bits, ch;
        memcpy(&sr, p + 4, sizeof(uint32_t));
        memcpy(&bits, p + 8, sizeof(uint16_t));
        memcpy(&ch, p + 10, sizeof(uint16_t));
        state->sample_rate = sr;
        state->bits = bits;
        state->channels = ch;
    } else if (strcmp(state->codec, "flac") == 0 && header_size >= 42) {
        /* FLAC streaminfo: sr at offset 18 (20 bits), channels at byte 20.5, bits at byte 21 partial */
        state->sample_rate = 48000;
        state->bits = 16;
        state->channels = 2;
    }
}

/* -----------------------------------------------------------------------
 * PCM dump – saves the first DUMP_BYTES bytes of received audio to a file.
 * Transfer ux0:data/snapcast/audio_dump.raw to PC and play with:
 *   sox -r 48000 -e signed-integer -b 16 -c 2 -L audio_dump.raw dump.wav
 * If it sounds clean there, the distortion is in the Vita audio output path.
 * If it also sounds distorted, the issue is in data reception.
 * Set AUDIO_DUMP_ENABLED to 0 to disable.
 * ----------------------------------------------------------------------- */
#define AUDIO_DUMP_ENABLED  0
#define DUMP_BYTES          (192000 * 5)   /* 5 seconds @ 48 kHz stereo 16 bit */
#define DUMP_FILE           "ux0:data/snapcast/audio_dump.raw"

static SceUID g_dump_fd    = -1;
static int    g_dump_bytes = 0;
static int    g_dump_done  = 0;

static void pcm_dump_init(void) {
    g_dump_done = 0;
    g_dump_bytes = 0;
    if (g_dump_fd >= 0) { sceIoClose(g_dump_fd); g_dump_fd = -1; }
#if AUDIO_DUMP_ENABLED
    sceIoRemove(DUMP_FILE);
    g_dump_fd = sceIoOpen(DUMP_FILE,
                          SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    main_log("[dump] opened %s  fd=%d\n", DUMP_FILE, g_dump_fd);
#endif
}

static void pcm_dump_write(const void *data, int size) {
#if AUDIO_DUMP_ENABLED
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
#else
    (void)data; (void)size;
#endif
}

/* Compute the effective sample rate correction and set it on the audio context.
   Mirrors the C++ client's Stream::setRealSampleRate + soft sync logic. */
static void compute_soft_sync(int sample_rate) {
    if (!age_buffer_full(&g_age_short)) {
        __atomic_store_n(&g_audio.correct_after_x_frames, 0, __ATOMIC_RELEASE);
        return;
    }

    int64_t mini_med  = age_buffer_median(&g_age_mini);
    int64_t short_med = g_median_short;
    int correction = 0;

    if (short_med > SOFT_SYNC_BEGIN_USEC &&
        mini_med > 50 &&
        short_med > 50)
    {
        /* We are behind (positive age): speed up by dropping frames */
        double rate = ((double)short_med / 100.0) * 0.00005;
        if (rate > 0.0005) rate = 0.0005;
        double real_rate = (double)sample_rate * (1.0 - rate);
        if (real_rate != (double)sample_rate) {
            double ratio = (double)sample_rate / real_rate;
            correction = (int)round(ratio / (ratio - 1.0));
        }
    }
    else if (short_med < -SOFT_SYNC_BEGIN_USEC &&
             mini_med < -50 &&
             short_med < -50)
    {
        /* We are ahead (negative age): slow down by duplicating frames */
        double rate = ((double)(-short_med) / 100.0) * 0.00005;
        if (rate > 0.0005) rate = 0.0005;
        double real_rate = (double)sample_rate * (1.0 + rate);
        if (real_rate != (double)sample_rate) {
            double ratio = (double)sample_rate / real_rate;
            correction = (int)round(ratio / (ratio - 1.0));
        }
    }

    __atomic_store_n(&g_audio.correct_after_x_frames, correction, __ATOMIC_RELEASE);
}

static void reset_sync_buffers(void) {
    age_buffer_clear(&g_age_mini);
    age_buffer_clear(&g_age_short);
    age_buffer_clear(&g_age_long);
    g_median_long = 0;
    g_median_short = 0;
    g_hard_sync = 1;
    g_state.in_hard_sync = 1;
    g_state.last_age_usec = 0;
    __atomic_store_n(&g_audio.correct_after_x_frames, 0, __ATOMIC_RELEASE);
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

    static int first_chunk_logged = 0;
    if (!first_chunk_logged) {
        main_log("[main] first wire_chunk: chunk_size=%u (expected 3840 @ 48kHz/20ms)\n",
                 chunk_size);
        first_chunk_logged = 1;
    }

    pcm_dump_write(pcm_data, (int)chunk_size);

    /* Hard sync mode: just fill the ring buffer.
     * The preroll mechanism holds the audio thread until effective_buf_ms of
     * audio has accumulated, giving the same lead-time as other Snapcast clients.
     * No age-based filtering here – age is meaningless until the ring is primed. */
    if (g_hard_sync) {
        audio_write(&g_audio, pcm_data, chunk_size);
        if (g_audio.prerolled) {
            g_hard_sync = 0;
            g_state.in_hard_sync = 0;
            age_buffer_clear(&g_age_mini);
            age_buffer_clear(&g_age_short);
            age_buffer_clear(&g_age_long);
            main_log("[sync] hard sync complete (prerolled)  diff=%lld us  buf=%d ms\n",
                     (long long)g_time_sync.diff_usec,
                     g_state.buffer_ms - g_state.config.latency_ms);
        }
        return;
    }

    /* ---- Normal (post-hard-sync) mode ---- */

    /* If time sync is not yet converged, write directly. */
    if (!time_sync_valid(&g_time_sync)) {
        audio_write(&g_audio, pcm_data, chunk_size);
        return;
    }

    int64_t chunk_ts_usec = (int64_t)ts_sec * 1000000 + ts_usec;
    int64_t server_now    = time_sync_server_now_usec(&g_time_sync);
    int effective_buf_ms  = g_state.buffer_ms - g_state.config.latency_ms;
    if (effective_buf_ms < 0) effective_buf_ms = 0;

    /* Age formula:
     *   age = server_now - chunk_ts - effective_buf_ms + ring_fill
     *
     * Derivation: the chunk will actually play at (server_now + ring_fill).
     * It *should* play at (chunk_ts + effective_buf_ms).
     * age = actual_play_time - intended_play_time.
     *
     * Steady state: server_now ≈ chunk_ts (chunk just received),
     *               ring_fill  ≈ effective_buf_ms (maintained by preroll).
     * → age ≈ 0  ✓
     *
     * age > 0: ring overfull or chunk arrived late  → slow down / drop
     * age < 0: ring underfull or chunk arrived early → speed up / nothing */
    int64_t ring_fill_usec = 0;
    if (g_audio.bytes_per_frame > 0 && g_audio.sample_rate > 0) {
        int readable = audio_available(&g_audio);
        ring_fill_usec = (int64_t)readable * 1000000
                         / ((int64_t)g_audio.sample_rate * g_audio.bytes_per_frame);
    }
    int64_t age_usec = server_now - chunk_ts_usec
                       - (int64_t)effective_buf_ms * 1000
                       + ring_fill_usec;

    /* Expose for GUI */
    g_state.last_age_usec = age_usec;

    static int first_synced_chunk = 0;
    if (!first_synced_chunk) {
        first_synced_chunk = 1;
        main_log("[sync] first normal chunk: age=%lld us  ring=%lld us"
                 "  buf=%d ms  diff=%lld us\n",
                 (long long)age_usec, (long long)ring_fill_usec,
                 effective_buf_ms, (long long)g_time_sync.diff_usec);
    }

    /* Drop chunks that are very late (> 500ms): we've fallen behind. */
    if (age_usec > 500000) {
        static int drop_count = 0;
        drop_count++;
        if (drop_count <= 3 || drop_count % 20 == 0)
            main_log("[sync] DROP #%d: age=%lld us > 500ms\n",
                     drop_count, (long long)age_usec);
        return;
    }

    /* Write chunk and track age for soft sync */
    audio_write(&g_audio, pcm_data, chunk_size);

    age_buffer_add(&g_age_mini,  age_usec);
    age_buffer_add(&g_age_short, age_usec);
    age_buffer_add(&g_age_long,  age_usec);

    /* Emergency hard sync if age is wildly off (> 2 seconds) */
    int64_t abs_age = age_usec < 0 ? -age_usec : age_usec;
    if (abs_age > 2000000) {
        main_log("[sync] emergency hard sync: |age|=%lld us > 2s\n",
                 (long long)abs_age);
        audio_flush(&g_audio);
        reset_sync_buffers();
        return;
    }

    /* Update medians and soft sync every ~1 second (50 chunks @ 20ms) */
    static int median_counter = 0;
    if (++median_counter >= 50) {
        median_counter = 0;

        if (age_buffer_full(&g_age_short))
            g_median_short = age_buffer_median(&g_age_short);
        if (age_buffer_full(&g_age_long))
            g_median_long = age_buffer_median(&g_age_long);

        /* Soft sync: nudge sample rate to correct gradual drift */
        compute_soft_sync(g_audio.sample_rate > 0 ? g_audio.sample_rate : 48000);

        main_log("[sync] age=%lld  short=%lld  long=%lld  corr=%d  diff=%lld\n",
                 (long long)age_usec,
                 (long long)g_median_short,
                 (long long)g_median_long,
                 __atomic_load_n(&g_audio.correct_after_x_frames, __ATOMIC_ACQUIRE),
                 (long long)g_time_sync.diff_usec);
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
        stream_thread_running = 0;
        return 0;
    }

    if (net_send_hello(net, &state->config) < 0) {
        strncpy(state->conn_error, "Failed to send hello", MAX_STR_LEN - 1);
        state->conn_state = CONN_ERROR;
        net_stream_disconnect(net);
        stream_thread_running = 0;
        return 0;
    }

    int codec_received = 0;
    int time_counter = 0;

    while (stream_thread_running) {
        BaseMessage hdr;
        char *payload = NULL;

        if (net_recv_message(net, &hdr, &payload) < 0) {
            if (stream_thread_running) {
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
                parse_codec_header(payload, hdr.size, state);
                codec_received = 1;
                {
                    int eff_buf = state->buffer_ms - state->config.latency_ms;
                    if (eff_buf < 50)  eff_buf = 50;
                    if (eff_buf > 5000) eff_buf = 5000;
                    main_log("[main] codec_header: codec='%s' rate=%d ch=%d bits=%d"
                             " bufMs=%d latMs=%d preroll=%dms\n",
                             state->codec, state->sample_rate, state->channels,
                             state->bits, state->buffer_ms,
                             state->config.latency_ms, eff_buf);
                    pcm_dump_init();
                    audio_flush(&g_audio);
                    reset_sync_buffers();
                    audio_configure(&g_audio,
                        state->sample_rate  > 0 ? state->sample_rate  : 48000,
                        state->channels     > 0 ? state->channels     : 2,
                        state->bits         > 0 ? state->bits         : 16,
                        eff_buf);
                    audio_start(&g_audio);
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

        /* Send time sync: every message during initial convergence,
           then every 50 messages (~1s) in steady state. */
        time_counter++;
        int sync_interval = time_sync_valid(&g_time_sync)
                            && g_time_sync.count >= 50 ? 50 : 1;
        if (time_counter >= sync_interval) {
            net_send_time(net);
            time_counter = 0;
        }
    }

    net_stream_disconnect(net);
    stream_thread_running = 0;
    return 0;
}

static void start_streaming(void) {
    if (stream_thread_running) return;
    stream_thread_running = 1;

    stream_thread_id = sceKernelCreateThread("stream_thread", stream_thread_func,
        0x10000100, 0x10000, 0, 0, NULL);
    if (stream_thread_id >= 0) {
        sceKernelStartThread(stream_thread_id, 0, NULL);
    } else {
        stream_thread_running = 0;
    }
}

static void stop_streaming(void) {
    stream_thread_running = 0;
    net_stream_disconnect(&g_net);
    if (stream_thread_id >= 0) {
        sceKernelWaitThreadEnd(stream_thread_id, NULL, NULL);
        sceKernelDeleteThread(stream_thread_id);
        stream_thread_id = -1;
    }
    audio_stop(&g_audio);
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

/* --- Connection management --- */

static void cleanup_connection(NetContext *net) {
    stop_streaming();
    net_control_disconnect(net);
}

static void do_connect(AppState *state, NetContext *net) {
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
    } else if (config_existed < 0) {
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

    if (audio_init(&g_audio) < 0) {
        strncpy(g_state.conn_error, "Audio init failed", MAX_STR_LEN - 1);
    }

    time_sync_init(&g_time_sync);
    age_buffer_init(&g_age_mini,  AGE_BUFFER_MINI);
    age_buffer_init(&g_age_short, AGE_BUFFER_SHORT);
    age_buffer_init(&g_age_long,  AGE_BUFFER_LONG);

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

        /* Handle connection state transitions */
        if (g_state.conn_state == CONN_CONNECTING) {
            do_connect(&g_state, &g_net);
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

        /* Check for stream thread failure */
        if (g_state.conn_state == CONN_CONNECTED && !stream_thread_running &&
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
                    main_log("[main] reconnected successfully\n");
                }
            }
        }

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
    age_buffer_free(&g_age_mini);
    age_buffer_free(&g_age_short);
    age_buffer_free(&g_age_long);
    gui_fini();
    net_fini();

    sceKernelExitProcess(0);
    return 0;
}
