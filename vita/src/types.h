#ifndef SNAPVITA_TYPES_H
#define SNAPVITA_TYPES_H

#include <stdint.h>

#define MAX_CLIENTS     32
#define MAX_GROUPS      16
#define MAX_STREAMS     16
#define MAX_ID_LEN      80
#define MAX_NAME_LEN    64
#define MAX_IP_LEN      48
#define MAX_STR_LEN     256
#define MAX_URL_LEN     512

#define SNAP_STREAM_PORT   1704
#define SNAP_CONTROL_PORT  1705
#define SNAP_PROTOCOL_VERSION 2
#define SNAP_CLIENT_NAME   "Snapclient"
#define SNAP_CLIENT_VERSION "0.28.0"

typedef enum {
    SCREEN_CONNECT = 0,
    SCREEN_PLAYER,
    SCREEN_DEVICES,
    SCREEN_GROUPS,
    SCREEN_SETTINGS,
    SCREEN_COUNT
} AppScreen;

typedef struct {
    int percent;
    int muted;
} Volume;

typedef struct {
    char title[MAX_STR_LEN];
    char artist[MAX_STR_LEN];
    char album[MAX_STR_LEN];
    char art_url[MAX_URL_LEN];
    double duration;
    double position;
} StreamMetadata;

typedef struct {
    int can_play;
    int can_pause;
    int can_go_next;
    int can_go_previous;
    int can_seek;
    int can_control;
    char playback_status[32];
    StreamMetadata metadata;
} StreamProperties;

typedef struct {
    char id[MAX_ID_LEN];
    char name[MAX_NAME_LEN];
    char host_name[MAX_NAME_LEN];
    char ip[MAX_IP_LEN];
    char os[MAX_STR_LEN];
    char arch[MAX_NAME_LEN];
    int connected;
    Volume volume;
    int latency;
    int instance;
} SnapClient;

typedef struct {
    char id[MAX_ID_LEN];
    char status[32];
    char name[MAX_NAME_LEN];
    char codec[32];
    char sample_format[32];
    char scheme[32];
    StreamProperties properties;
} SnapStream;

typedef struct {
    char id[MAX_ID_LEN];
    char name[MAX_NAME_LEN];
    int muted;
    char stream_id[MAX_ID_LEN];
    SnapClient clients[MAX_CLIENTS];
    int client_count;
} SnapGroup;

typedef struct {
    SnapGroup groups[MAX_GROUPS];
    int group_count;
    SnapStream streams[MAX_STREAMS];
    int stream_count;
    char host_name[MAX_NAME_LEN];
    char version[MAX_NAME_LEN];
} SnapServer;

typedef struct {
    char server_ip[MAX_IP_LEN];
    int stream_port;
    int control_port;
    char client_name[MAX_NAME_LEN];
    int latency_ms;
} AppConfig;

typedef enum {
    CONN_DISCONNECTED = 0,
    CONN_CONNECTING,
    CONN_CONNECTED,
    CONN_RECONNECTING,
    CONN_ERROR
} ConnectionState;

typedef struct {
    ConnectionState conn_state;
    char conn_error[MAX_STR_LEN];
    AppScreen current_screen;
    SnapServer server;
    char our_client_id[MAX_ID_LEN];
    AppConfig config;

    int volume_percent;
    int volume_muted;
    int buffer_ms;
    char codec[32];
    int sample_rate;
    int bits;
    int channels;

    int selected_item;
    int selected_group;
    int ime_active;
    int ime_field;

    int running;

    int auto_reconnect;
    int reconnect_attempts;
    int reconnect_timer;
    int reconnect_delay_frames;

    /* Time sync status (updated from stream thread) */
    volatile int64_t time_diff_usec;  /* server_time - vita_time in µs */
    volatile int      time_sync_count; /* number of samples collected */
    volatile int64_t  last_age_usec;   /* most recent chunk age in µs */
    volatile int      in_hard_sync;    /* 1 while hard sync is active */

    /* Network bandwidth (updated from stream thread) */
    volatile int wire_kbps; /* measured compressed wire bitrate in kbps */
} AppState;

/* Binary protocol message types */
typedef enum {
    MSG_BASE          = 0,
    MSG_CODEC_HEADER  = 1,
    MSG_WIRE_CHUNK    = 2,
    MSG_SERVER_SETTINGS = 3,
    MSG_TIME          = 4,
    MSG_HELLO         = 5,
    MSG_CLIENT_INFO   = 7,
    MSG_ERROR         = 8
} MsgType;

#pragma pack(push, 1)
typedef struct {
    uint16_t type;
    uint16_t id;
    uint16_t refers_to;
    int32_t  sent_sec;
    int32_t  sent_usec;
    int32_t  received_sec;
    int32_t  received_usec;
    uint32_t size;
} BaseMessage;
#pragma pack(pop)

#define BASE_MSG_SIZE 26

#endif /* SNAPVITA_TYPES_H */
