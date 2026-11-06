#include "config.h"
#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

/* Owned by the UI thread. Compare meaningful settings, not struct padding or
 * unused profile slots, before touching storage. */
static AppConfig saved_config;
static int saved_config_valid;

static int config_equal(const AppConfig *a, const AppConfig *b) {
    if (strcmp(a->server_ip, b->server_ip) ||
        strcmp(a->client_name, b->client_name) ||
        a->stream_port != b->stream_port || a->control_port != b->control_port ||
        a->latency_ms != b->latency_ms || a->net_profile_count != b->net_profile_count)
        return 0;
    for (int i = 0; i < a->net_profile_count; ++i) {
        if (strcmp(a->net_profiles[i].net_prefix, b->net_profiles[i].net_prefix) ||
            strcmp(a->net_profiles[i].server_ip, b->net_profiles[i].server_ip))
            return 0;
    }
    return 1;
}

void config_default(AppConfig *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    strncpy(cfg->server_ip, "192.168.1.100", MAX_IP_LEN - 1);
    cfg->stream_port = SNAP_STREAM_PORT;
    cfg->control_port = SNAP_CONTROL_PORT;
    strncpy(cfg->client_name, "PS Vita", MAX_NAME_LEN - 1);
    cfg->latency_ms = 0;
    cfg->net_profile_count = 0;
}

int config_load(AppConfig *cfg) {
    saved_config_valid = 0;
    config_default(cfg);

    SceUID fd = sceIoOpen(CONFIG_FILE, SCE_O_RDONLY, 0);
    if (fd < 0) return -1;

    char buf[4096];
    int bytes = sceIoRead(fd, buf, sizeof(buf) - 1);
    sceIoClose(fd);
    if (bytes <= 0) return -1;
    buf[bytes] = '\0';

    JsonNode *root = json_parse(buf);
    if (!root) return -1;
    if (root->type != JSON_OBJECT) { json_free(root); return -1; }

    const char *ip = json_get_string(root, "server_ip", NULL);
    if (ip) strncpy(cfg->server_ip, ip, MAX_IP_LEN - 1);

    cfg->stream_port = json_get_int(root, "stream_port", SNAP_STREAM_PORT);
    cfg->control_port = json_get_int(root, "control_port", SNAP_CONTROL_PORT);

    const char *name = json_get_string(root, "client_name", NULL);
    if (name) strncpy(cfg->client_name, name, MAX_NAME_LEN - 1);

    cfg->latency_ms = json_get_int(root, "latency_ms", 0);

    JsonNode *profiles = json_get(root, "net_profiles");
    if (profiles && profiles->type == JSON_ARRAY) {
        int count = json_array_size(profiles);
        if (count > MAX_NET_PROFILES) count = MAX_NET_PROFILES;
        cfg->net_profile_count = 0;
        for (int i = 0; i < count; i++) {
            JsonNode *entry = json_array_get(profiles, i);
            if (!entry) continue;
            const char *prefix = json_get_string(entry, "prefix", NULL);
            const char *sip    = json_get_string(entry, "server_ip", NULL);
            if (prefix && sip) {
                strncpy(cfg->net_profiles[cfg->net_profile_count].net_prefix,
                        prefix, MAX_IP_LEN - 1);
                strncpy(cfg->net_profiles[cfg->net_profile_count].server_ip,
                        sip, MAX_IP_LEN - 1);
                cfg->net_profile_count++;
            }
        }
    }

    json_free(root);
    saved_config = *cfg;
    saved_config_valid = 1;
    return 0;
}

int config_save(const AppConfig *cfg) {
    if (cfg->net_profile_count < 0 || cfg->net_profile_count > MAX_NET_PROFILES) return -1;
    if (saved_config_valid && config_equal(cfg, &saved_config)) return 0;
    AppConfig snapshot = *cfg;
    cfg = &snapshot;

    JsonNode *root = json_new_object();
    json_object_add(root, "server_ip",    json_new_string(cfg->server_ip));
    json_object_add(root, "stream_port",  json_new_int(cfg->stream_port));
    json_object_add(root, "control_port", json_new_int(cfg->control_port));
    json_object_add(root, "client_name",  json_new_string(cfg->client_name));
    json_object_add(root, "latency_ms",   json_new_int(cfg->latency_ms));

    JsonNode *profiles = json_new_array();
    for (int i = 0; i < cfg->net_profile_count; i++) {
        JsonNode *entry = json_new_object();
        json_object_add(entry, "prefix",
                        json_new_string(cfg->net_profiles[i].net_prefix));
        json_object_add(entry, "server_ip",
                        json_new_string(cfg->net_profiles[i].server_ip));
        json_array_add(profiles, entry);
    }
    json_object_add(root, "net_profiles", profiles);

    char *json_str = json_serialize(root);
    json_free(root);
    if (!json_str) return -1;

    sceIoMkdir(CONFIG_DIR, 0777);
    SceUID fd = sceIoOpen(CONFIG_FILE, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0) { free(json_str); return -1; }

    /* A truncated or failed write must never be cached as a successful save. */
    saved_config_valid = 0;
    size_t length = strlen(json_str), written = 0;
    while (written < length) {
        SceSSize result = sceIoWrite(fd, json_str + written, length - written);
        if (result <= 0) break;
        written += (size_t)result;
    }
    int close_result = sceIoClose(fd);
    free(json_str);
    if (written != length || close_result < 0) return -1;
    saved_config = snapshot;
    saved_config_valid = 1;
    return 0;
}
