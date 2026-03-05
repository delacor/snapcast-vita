#include "types.h"
#include "json.h"
#include "config.h"
#include "network.h"
#include "audio.h"
#include "gui.h"

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

static void main_log(const char *fmt, ...) {
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
#define AUDIO_DUMP_ENABLED  1
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

static void handle_wire_chunk(const char *payload, uint32_t size) {
    if (size < 12) return;

    /* Wire chunk body: timestamp.sec(4) + timestamp.usec(4) + size(4) + data */
    uint32_t chunk_size;
    memcpy(&chunk_size, payload + 8, sizeof(uint32_t));
    if (chunk_size > size - 12) return;

    const char *pcm_data = payload + 12;

    /* Log first chunk size to verify expected value (3840 B @ 48kHz/20ms) */
    static int first_chunk_logged = 0;
    if (!first_chunk_logged) {
        main_log("[main] first wire_chunk: chunk_size=%u (expected 3840 @ 48kHz/20ms)\n",
                 chunk_size);
        first_chunk_logged = 1;
    }

    pcm_dump_write(pcm_data, (int)chunk_size);
    audio_write(&g_audio, pcm_data, chunk_size);
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

        switch (hdr.type) {
            case MSG_SERVER_SETTINGS:
                parse_server_settings(payload, hdr.size, state);
                break;

            case MSG_CODEC_HEADER:
                parse_codec_header(payload, hdr.size, state);
                codec_received = 1;
                main_log("[main] codec_header: codec='%s' rate=%d ch=%d bits=%d bufMs=%d\n",
                         state->codec, state->sample_rate, state->channels,
                         state->bits, state->buffer_ms);
                pcm_dump_init();
                audio_flush(&g_audio);
                audio_configure(&g_audio,
                    state->sample_rate  > 0 ? state->sample_rate  : 48000,
                    state->channels     > 0 ? state->channels     : 2,
                    state->bits         > 0 ? state->bits         : 16,
                    50);
                audio_start(&g_audio);
                break;

            case MSG_WIRE_CHUNK:
                if (codec_received)
                    handle_wire_chunk(payload, hdr.size);
                break;

            case MSG_TIME:
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

        /* Periodically send time sync */
        time_counter++;
        if (time_counter >= 50) {
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
    audio_flush(&g_audio);

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
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

    sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    sceSysmoduleLoadModule(SCE_SYSMODULE_IME);

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
    gui_fini();
    net_fini();

    sceKernelExitProcess(0);
    return 0;
}
