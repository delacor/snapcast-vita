#include "network.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/rtc.h>

static void *net_memory = NULL;

int net_init(void) {
    if (net_memory) return 0;

    net_memory = malloc(1 * 1024 * 1024);
    if (!net_memory) return -1;

    SceNetInitParam param;
    param.memory = net_memory;
    param.size = 1 * 1024 * 1024;
    param.flags = 0;

    if (sceNetInit(&param) < 0) { free(net_memory); net_memory = NULL; return -1; }
    if (sceNetCtlInit() < 0) { sceNetTerm(); free(net_memory); net_memory = NULL; return -1; }

    return 0;
}

void net_fini(void) {
    sceNetCtlTerm();
    sceNetTerm();
    free(net_memory);
    net_memory = NULL;
}

static int tcp_connect(const char *ip, int port) {
    int sock = sceNetSocket("snap", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0);
    if (sock < 0) return -1;

    SceNetSockaddrIn addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_len = sizeof(addr);
    addr.sin_family = SCE_NET_AF_INET;
    addr.sin_port = sceNetHtons((unsigned short)port);
    sceNetInetPton(SCE_NET_AF_INET, ip, &addr.sin_addr);

    if (sceNetConnect(sock, (SceNetSockaddr *)&addr, sizeof(addr)) < 0) {
        sceNetSocketClose(sock);
        return -1;
    }

    return sock;
}

static int tcp_set_nonblock(int sock) {
    int val = 1;
    return sceNetSetsockopt(sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, &val, sizeof(val));
}

static int tcp_send_all(int sock, const void *data, int len) {
    const char *p = (const char *)data;
    int remaining = len;
    while (remaining > 0) {
        int sent = sceNetSend(sock, p, remaining, 0);
        if (sent <= 0) return -1;
        p += sent;
        remaining -= sent;
    }
    return len;
}

static int tcp_recv_all(int sock, void *buf, int len) {
    char *p = (char *)buf;
    int remaining = len;
    while (remaining > 0) {
        int got = sceNetRecv(sock, p, remaining, 0);
        if (got <= 0) return -1;
        p += got;
        remaining -= got;
    }
    return len;
}

static void get_time(int32_t *sec, int32_t *usec) {
    SceRtcTick tick;
    sceRtcGetCurrentTick(&tick);
    *sec = (int32_t)(tick.tick / 1000000);
    *usec = (int32_t)(tick.tick % 1000000);
}

/* --- Streaming connection (binary protocol) --- */

int net_stream_connect(NetContext *ctx, const char *ip, int port) {
    ctx->stream_sock = tcp_connect(ip, port);
    if (ctx->stream_sock < 0) return -1;
    ctx->recv_len = 0;
    ctx->time_msg_id = 0;
    return 0;
}

void net_stream_disconnect(NetContext *ctx) {
    if (ctx->stream_sock >= 0) {
        sceNetSocketClose(ctx->stream_sock);
        ctx->stream_sock = -1;
    }
}

int net_send_hello(NetContext *ctx, const AppConfig *cfg) {
    if (ctx->stream_sock < 0) return -1;

    SceNetCtlInfo info;
    char mac_str[32] = "00:00:00:00:00:00";
    char ip_str[MAX_IP_LEN] = "";
    if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &info) >= 0)
        strncpy(ip_str, info.ip_address, MAX_IP_LEN - 1);

    JsonNode *hello = json_new_object();
    json_object_add(hello, "Arch", json_new_string("armv7"));
    json_object_add(hello, "ClientName", json_new_string(SNAP_CLIENT_NAME));
    json_object_add(hello, "HostName", json_new_string(cfg->client_name));
    json_object_add(hello, "ID", json_new_string(mac_str));
    json_object_add(hello, "Instance", json_new_int(1));
    json_object_add(hello, "MAC", json_new_string(mac_str));
    json_object_add(hello, "OS", json_new_string("PS Vita"));
    json_object_add(hello, "SnapStreamProtocolVersion", json_new_int(SNAP_PROTOCOL_VERSION));
    json_object_add(hello, "Version", json_new_string(SNAP_CLIENT_VERSION));

    char *json_str = json_serialize(hello);
    json_free(hello);
    if (!json_str) return -1;

    uint32_t json_len = (uint32_t)strlen(json_str);

    BaseMessage hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.type = MSG_HELLO;
    hdr.id = 0;
    hdr.refers_to = 0;
    get_time(&hdr.sent_sec, &hdr.sent_usec);
    hdr.size = sizeof(uint32_t) + json_len;

    int ret = tcp_send_all(ctx->stream_sock, &hdr, BASE_MSG_SIZE);
    if (ret < 0) { free(json_str); return -1; }

    ret = tcp_send_all(ctx->stream_sock, &json_len, sizeof(uint32_t));
    if (ret < 0) { free(json_str); return -1; }

    ret = tcp_send_all(ctx->stream_sock, json_str, json_len);
    free(json_str);
    return ret < 0 ? -1 : 0;
}

int net_send_time(NetContext *ctx) {
    if (ctx->stream_sock < 0) return -1;

    BaseMessage hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.type = MSG_TIME;
    hdr.id = (uint16_t)(++ctx->time_msg_id);
    get_time(&hdr.sent_sec, &hdr.sent_usec);
    hdr.size = 2 * sizeof(int32_t);

    int32_t latency[2] = {0, 0};

    int ret = tcp_send_all(ctx->stream_sock, &hdr, BASE_MSG_SIZE);
    if (ret < 0) return -1;
    return tcp_send_all(ctx->stream_sock, latency, sizeof(latency));
}

int net_recv_message(NetContext *ctx, BaseMessage *hdr, char **payload) {
    if (ctx->stream_sock < 0) return -1;

    if (tcp_recv_all(ctx->stream_sock, hdr, BASE_MSG_SIZE) < 0)
        return -1;

    if (hdr->size > 1000000) return -1;

    *payload = NULL;
    if (hdr->size > 0) {
        *payload = (char *)malloc(hdr->size);
        if (!*payload) return -1;
        if (tcp_recv_all(ctx->stream_sock, *payload, hdr->size) < 0) {
            free(*payload);
            *payload = NULL;
            return -1;
        }
    }

    int32_t sec, usec;
    get_time(&sec, &usec);
    hdr->received_sec = sec;
    hdr->received_usec = usec;

    return 0;
}

/* --- Control connection (JSON-RPC) --- */

int net_control_connect(NetContext *ctx, const char *ip, int port) {
    ctx->control_sock = tcp_connect(ip, port);
    if (ctx->control_sock < 0) return -1;
    tcp_set_nonblock(ctx->control_sock);
    ctx->ctrl_len = 0;
    ctx->rpc_id = 0;
    return 0;
}

void net_control_disconnect(NetContext *ctx) {
    if (ctx->control_sock >= 0) {
        sceNetSocketClose(ctx->control_sock);
        ctx->control_sock = -1;
    }
}

static int rpc_send(NetContext *ctx, JsonNode *req) {
    if (ctx->control_sock < 0) return -1;

    char *str = json_serialize(req);
    if (!str) return -1;

    size_t len = strlen(str);
    char *buf = (char *)malloc(len + 2);
    memcpy(buf, str, len);
    buf[len] = '\n';
    buf[len + 1] = '\0';
    free(str);

    int ret = tcp_send_all(ctx->control_sock, buf, (int)(len + 1));
    free(buf);
    return ret < 0 ? -1 : 0;
}

static JsonNode *rpc_build_request(NetContext *ctx, const char *method, JsonNode *params) {
    JsonNode *req = json_new_object();
    json_object_add(req, "id", json_new_int(++ctx->rpc_id));
    json_object_add(req, "jsonrpc", json_new_string("2.0"));
    json_object_add(req, "method", json_new_string(method));
    if (params)
        json_object_add(req, "params", params);
    return req;
}

int net_rpc_get_status(NetContext *ctx) {
    JsonNode *req = rpc_build_request(ctx, "Server.GetStatus", NULL);
    int ret = rpc_send(ctx, req);
    json_free(req);
    return ret;
}

int net_rpc_set_volume(NetContext *ctx, const char *client_id, int percent, int muted) {
    JsonNode *vol = json_new_object();
    json_object_add(vol, "percent", json_new_int(percent));
    json_object_add(vol, "muted", json_new_bool(muted));

    JsonNode *params = json_new_object();
    json_object_add(params, "id", json_new_string(client_id));
    json_object_add(params, "volume", vol);

    JsonNode *req = rpc_build_request(ctx, "Client.SetVolume", params);
    int ret = rpc_send(ctx, req);
    json_free(req);
    return ret;
}

int net_rpc_set_latency(NetContext *ctx, const char *client_id, int latency) {
    JsonNode *params = json_new_object();
    json_object_add(params, "id", json_new_string(client_id));
    json_object_add(params, "latency", json_new_int(latency));

    JsonNode *req = rpc_build_request(ctx, "Client.SetLatency", params);
    int ret = rpc_send(ctx, req);
    json_free(req);
    return ret;
}

int net_rpc_set_name(NetContext *ctx, const char *client_id, const char *name) {
    JsonNode *params = json_new_object();
    json_object_add(params, "id", json_new_string(client_id));
    json_object_add(params, "name", json_new_string(name));

    JsonNode *req = rpc_build_request(ctx, "Client.SetName", params);
    int ret = rpc_send(ctx, req);
    json_free(req);
    return ret;
}

int net_rpc_set_group_mute(NetContext *ctx, const char *group_id, int mute) {
    JsonNode *params = json_new_object();
    json_object_add(params, "id", json_new_string(group_id));
    json_object_add(params, "mute", json_new_bool(mute));

    JsonNode *req = rpc_build_request(ctx, "Group.SetMute", params);
    int ret = rpc_send(ctx, req);
    json_free(req);
    return ret;
}

int net_rpc_set_group_stream(NetContext *ctx, const char *group_id, const char *stream_id) {
    JsonNode *params = json_new_object();
    json_object_add(params, "id", json_new_string(group_id));
    json_object_add(params, "stream_id", json_new_string(stream_id));

    JsonNode *req = rpc_build_request(ctx, "Group.SetStream", params);
    int ret = rpc_send(ctx, req);
    json_free(req);
    return ret;
}

int net_rpc_set_group_clients(NetContext *ctx, const char *group_id,
                               const char **client_ids, int count) {
    /* Build JSON array manually via the child linked list */
    JsonNode *arr = (JsonNode *)calloc(1, sizeof(JsonNode));
    arr->type = JSON_ARRAY;
    JsonNode *tail = NULL;
    for (int i = 0; i < count; i++) {
        JsonNode *s = json_new_string(client_ids[i]);
        if (!arr->child) { arr->child = s; }
        else { tail->next = s; }
        tail = s;
    }

    JsonNode *params = json_new_object();
    json_object_add(params, "id", json_new_string(group_id));
    json_object_add(params, "clients", arr);

    JsonNode *req = rpc_build_request(ctx, "Group.SetClients", params);
    int ret = rpc_send(ctx, req);
    json_free(req);
    return ret;
}

int net_rpc_stream_control(NetContext *ctx, const char *stream_id, const char *command) {
    JsonNode *params = json_new_object();
    json_object_add(params, "id", json_new_string(stream_id));
    json_object_add(params, "command", json_new_string(command));

    JsonNode *req = rpc_build_request(ctx, "Stream.Control", params);
    int ret = rpc_send(ctx, req);
    json_free(req);
    return ret;
}

int net_rpc_poll(NetContext *ctx, char **line_out) {
    *line_out = NULL;
    if (ctx->control_sock < 0) return 0;

    int space = JSONRPC_BUF_SIZE - ctx->ctrl_len - 1;
    if (space > 0) {
        int got = sceNetRecv(ctx->control_sock, ctx->ctrl_buf + ctx->ctrl_len, space, 0);
        if (got > 0) ctx->ctrl_len += got;
    }

    char *nl = memchr(ctx->ctrl_buf, '\n', ctx->ctrl_len);
    if (!nl) return 0;

    int line_len = (int)(nl - ctx->ctrl_buf);
    *line_out = (char *)malloc(line_len + 1);
    memcpy(*line_out, ctx->ctrl_buf, line_len);
    (*line_out)[line_len] = '\0';

    int remaining = ctx->ctrl_len - line_len - 1;
    if (remaining > 0)
        memmove(ctx->ctrl_buf, nl + 1, remaining);
    ctx->ctrl_len = remaining;

    return 1;
}

/* --- State parsing --- */

static void parse_volume(JsonNode *vol_obj, Volume *vol) {
    vol->percent = json_get_int(vol_obj, "percent", 100);
    vol->muted = json_get_bool(vol_obj, "muted", 0);
}

static void parse_client(JsonNode *cj, SnapClient *client) {
    memset(client, 0, sizeof(*client));

    const char *id = json_get_string(cj, "id", "");
    strncpy(client->id, id, MAX_ID_LEN - 1);
    client->connected = json_get_bool(cj, "connected", 0);

    JsonNode *config = json_get(cj, "config");
    if (config) {
        const char *name = json_get_string(config, "name", "");
        strncpy(client->name, name, MAX_NAME_LEN - 1);
        client->latency = json_get_int(config, "latency", 0);
        client->instance = json_get_int(config, "instance", 1);

        JsonNode *vol = json_get(config, "volume");
        if (vol) parse_volume(vol, &client->volume);
    }

    JsonNode *host = json_get(cj, "host");
    if (host) {
        const char *hname = json_get_string(host, "name", "");
        strncpy(client->host_name, hname, MAX_NAME_LEN - 1);
        const char *ip = json_get_string(host, "ip", "");
        strncpy(client->ip, ip, MAX_IP_LEN - 1);
        const char *os = json_get_string(host, "os", "");
        strncpy(client->os, os, MAX_STR_LEN - 1);
        const char *arch = json_get_string(host, "arch", "");
        strncpy(client->arch, arch, MAX_NAME_LEN - 1);
    }
}

static void parse_stream(JsonNode *sj, SnapStream *stream) {
    memset(stream, 0, sizeof(*stream));

    const char *id = json_get_string(sj, "id", "");
    strncpy(stream->id, id, MAX_ID_LEN - 1);
    const char *status = json_get_string(sj, "status", "unknown");
    strncpy(stream->status, status, sizeof(stream->status) - 1);

    JsonNode *uri = json_get(sj, "uri");
    if (uri) {
        const char *scheme = json_get_string(uri, "scheme", "");
        strncpy(stream->scheme, scheme, sizeof(stream->scheme) - 1);

        JsonNode *query = json_get(uri, "query");
        if (query) {
            const char *name = json_get_string(query, "name", "");
            strncpy(stream->name, name, MAX_NAME_LEN - 1);
            const char *codec = json_get_string(query, "codec", "");
            strncpy(stream->codec, codec, sizeof(stream->codec) - 1);
            const char *sf = json_get_string(query, "sampleformat", "");
            strncpy(stream->sample_format, sf, sizeof(stream->sample_format) - 1);
        }
    }

    JsonNode *props = json_get(sj, "properties");
    if (props) net_parse_stream_properties(props, stream);
}

/* Extract a string value that may be either a JSON string or an array of
 * strings (Snapcast/MPRIS sends artist, albumArtist, genre as arrays). */
static void json_extract_string_or_array(JsonNode *obj, const char *key,
                                         char *dest, int max_len) {
    JsonNode *n = json_get(obj, key);
    if (!n) return;

    if (n->type == JSON_STRING && n->str_val) {
        strncpy(dest, n->str_val, max_len - 1);
        dest[max_len - 1] = '\0';
    } else if (n->type == JSON_ARRAY) {
        int pos = 0;
        for (JsonNode *e = n->child; e && pos < max_len - 1; e = e->next) {
            if (e->type == JSON_STRING && e->str_val) {
                if (pos > 0 && pos + 2 < max_len - 1) {
                    dest[pos++] = ',';
                    dest[pos++] = ' ';
                }
                int len = (int)strlen(e->str_val);
                if (len > max_len - 1 - pos) len = max_len - 1 - pos;
                memcpy(dest + pos, e->str_val, len);
                pos += len;
            }
        }
        dest[pos] = '\0';
    }
}

void net_parse_stream_properties(JsonNode *props, SnapStream *stream) {
    if (!props) return;

    stream->properties.can_play = json_get_bool(props, "canPlay", 0);
    stream->properties.can_pause = json_get_bool(props, "canPause", 0);
    stream->properties.can_go_next = json_get_bool(props, "canGoNext", 0);
    stream->properties.can_go_previous = json_get_bool(props, "canGoPrevious", 0);
    stream->properties.can_seek = json_get_bool(props, "canSeek", 0);
    stream->properties.can_control = json_get_bool(props, "canControl", 0);

    const char *ps = json_get_string(props, "playbackStatus", "");
    strncpy(stream->properties.playback_status, ps,
            sizeof(stream->properties.playback_status) - 1);

    /* Position lives at properties level (MPRIS property, not metadata) */
    stream->properties.metadata.position = json_get_number(props, "position", 0.0);

    JsonNode *meta = json_get(props, "metadata");
    if (meta) {
        const char *t = json_get_string(meta, "title",
                        json_get_string(meta, "track", ""));
        strncpy(stream->properties.metadata.title, t, MAX_STR_LEN - 1);

        json_extract_string_or_array(meta, "artist",
                                     stream->properties.metadata.artist, MAX_STR_LEN);
        if (!stream->properties.metadata.artist[0])
            json_extract_string_or_array(meta, "albumArtist",
                                         stream->properties.metadata.artist, MAX_STR_LEN);

        const char *al = json_get_string(meta, "album", "");
        strncpy(stream->properties.metadata.album, al, MAX_STR_LEN - 1);

        const char *art = json_get_string(meta, "artUrl", "");
        strncpy(stream->properties.metadata.art_url, art, MAX_URL_LEN - 1);

        stream->properties.metadata.duration = json_get_number(meta, "duration", 0.0);
    }
}

static void parse_group(JsonNode *gj, SnapGroup *group) {
    memset(group, 0, sizeof(*group));

    const char *id = json_get_string(gj, "id", "");
    strncpy(group->id, id, MAX_ID_LEN - 1);
    const char *name = json_get_string(gj, "name", "");
    strncpy(group->name, name, MAX_NAME_LEN - 1);
    group->muted = json_get_bool(gj, "muted", 0);
    const char *sid = json_get_string(gj, "stream_id", "");
    strncpy(group->stream_id, sid, MAX_ID_LEN - 1);

    JsonNode *clients = json_get(gj, "clients");
    if (clients && clients->type == JSON_ARRAY) {
        int count = json_array_size(clients);
        if (count > MAX_CLIENTS) count = MAX_CLIENTS;
        group->client_count = count;
        for (int i = 0; i < count; i++) {
            JsonNode *cj = json_array_get(clients, i);
            if (cj) parse_client(cj, &group->clients[i]);
        }
    }
}

void net_parse_server_status(JsonNode *result, SnapServer *server) {
    if (!result) return;

    JsonNode *srv = json_get(result, "server");
    if (!srv) return;

    memset(server, 0, sizeof(*server));

    JsonNode *host_info = json_get(srv, "server");
    if (host_info) {
        JsonNode *host = json_get(host_info, "host");
        if (host) {
            const char *hname = json_get_string(host, "name", "");
            strncpy(server->host_name, hname, MAX_NAME_LEN - 1);
        }
        JsonNode *ss = json_get(host_info, "snapserver");
        if (ss) {
            const char *ver = json_get_string(ss, "version", "");
            strncpy(server->version, ver, MAX_NAME_LEN - 1);
        }
    }

    JsonNode *groups = json_get(srv, "groups");
    if (groups && groups->type == JSON_ARRAY) {
        int count = json_array_size(groups);
        if (count > MAX_GROUPS) count = MAX_GROUPS;
        server->group_count = count;
        for (int i = 0; i < count; i++) {
            JsonNode *gj = json_array_get(groups, i);
            if (gj) parse_group(gj, &server->groups[i]);
        }
    }

    JsonNode *streams = json_get(srv, "streams");
    if (streams && streams->type == JSON_ARRAY) {
        int count = json_array_size(streams);
        if (count > MAX_STREAMS) count = MAX_STREAMS;
        server->stream_count = count;
        for (int i = 0; i < count; i++) {
            JsonNode *sj = json_array_get(streams, i);
            if (sj) parse_stream(sj, &server->streams[i]);
        }
    }
}
