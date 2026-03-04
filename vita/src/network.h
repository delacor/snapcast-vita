#ifndef SNAPVITA_NETWORK_H
#define SNAPVITA_NETWORK_H

#include "types.h"
#include "json.h"
#include <psp2/types.h>

#define NET_RECV_BUF_SIZE  (256 * 1024)
#define JSONRPC_BUF_SIZE   (64 * 1024)

typedef struct {
    int stream_sock;
    int control_sock;
    int net_inited;

    char recv_buf[NET_RECV_BUF_SIZE];
    int recv_len;

    char ctrl_buf[JSONRPC_BUF_SIZE];
    int ctrl_len;

    int rpc_id;
    int time_msg_id;
} NetContext;

/* Global network init/fini */
int  net_init(void);
void net_fini(void);

/* Connect/disconnect streaming socket (binary protocol, port 1704) */
int  net_stream_connect(NetContext *ctx, const char *ip, int port);
void net_stream_disconnect(NetContext *ctx);

/* Connect/disconnect control socket (JSON-RPC, port 1705) */
int  net_control_connect(NetContext *ctx, const char *ip, int port);
void net_control_disconnect(NetContext *ctx);

/* Binary protocol */
int  net_send_hello(NetContext *ctx, const AppConfig *cfg);
int  net_send_time(NetContext *ctx);
int  net_recv_message(NetContext *ctx, BaseMessage *hdr, char **payload);

/* JSON-RPC requests */
int  net_rpc_get_status(NetContext *ctx);
int  net_rpc_set_volume(NetContext *ctx, const char *client_id, int percent, int muted);
int  net_rpc_set_latency(NetContext *ctx, const char *client_id, int latency);
int  net_rpc_set_name(NetContext *ctx, const char *client_id, const char *name);
int  net_rpc_set_group_mute(NetContext *ctx, const char *group_id, int mute);
int  net_rpc_set_group_stream(NetContext *ctx, const char *group_id, const char *stream_id);
int  net_rpc_set_group_clients(NetContext *ctx, const char *group_id,
                                const char **client_ids, int count);
int  net_rpc_stream_control(NetContext *ctx, const char *stream_id, const char *command);

/* Read/parse JSON-RPC responses and notifications (non-blocking) */
int  net_rpc_poll(NetContext *ctx, char **line_out);

/* Parse server state from JSON-RPC Server.GetStatus result */
void net_parse_server_status(JsonNode *result, SnapServer *server);
void net_parse_stream_properties(JsonNode *props, SnapStream *stream);

#endif /* SNAPVITA_NETWORK_H */
