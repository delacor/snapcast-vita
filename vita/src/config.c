#include "config.h"
#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

void config_default(AppConfig *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    strncpy(cfg->server_ip, "192.168.1.100", MAX_IP_LEN - 1);
    cfg->stream_port = SNAP_STREAM_PORT;
    cfg->control_port = SNAP_CONTROL_PORT;
    strncpy(cfg->client_name, "PS Vita", MAX_NAME_LEN - 1);
    cfg->latency_ms = 0;
}

int config_load(AppConfig *cfg) {
    config_default(cfg);

    SceUID fd = sceIoOpen(CONFIG_FILE, SCE_O_RDONLY, 0);
    if (fd < 0) return -1;

    char buf[2048];
    int bytes = sceIoRead(fd, buf, sizeof(buf) - 1);
    sceIoClose(fd);
    if (bytes <= 0) return -1;
    buf[bytes] = '\0';

    JsonNode *root = json_parse(buf);
    if (!root) return -1;

    const char *ip = json_get_string(root, "server_ip", NULL);
    if (ip) strncpy(cfg->server_ip, ip, MAX_IP_LEN - 1);

    cfg->stream_port = json_get_int(root, "stream_port", SNAP_STREAM_PORT);
    cfg->control_port = json_get_int(root, "control_port", SNAP_CONTROL_PORT);

    const char *name = json_get_string(root, "client_name", NULL);
    if (name) strncpy(cfg->client_name, name, MAX_NAME_LEN - 1);

    cfg->latency_ms = json_get_int(root, "latency_ms", 0);

    json_free(root);
    return 0;
}

int config_save(const AppConfig *cfg) {
    sceIoMkdir(CONFIG_DIR, 0777);

    JsonNode *root = json_new_object();
    json_object_add(root, "server_ip", json_new_string(cfg->server_ip));
    json_object_add(root, "stream_port", json_new_int(cfg->stream_port));
    json_object_add(root, "control_port", json_new_int(cfg->control_port));
    json_object_add(root, "client_name", json_new_string(cfg->client_name));
    json_object_add(root, "latency_ms", json_new_int(cfg->latency_ms));

    char *json_str = json_serialize(root);
    json_free(root);
    if (!json_str) return -1;

    SceUID fd = sceIoOpen(CONFIG_FILE, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0) { free(json_str); return -1; }

    sceIoWrite(fd, json_str, strlen(json_str));
    sceIoClose(fd);
    free(json_str);
    return 0;
}
