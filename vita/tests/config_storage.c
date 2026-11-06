/* Exercise the real configuration persistence against a counted fake file. */
#include "../src/config.c"
#include <assert.h>

static char disk[4096];
static int exists, disk_size, position;
static int opens, writes, mkdirs;
static int fail_open, fail_write, fail_close, write_limit;

int sceIoMkdir(const char *path, SceMode mode) { mkdirs++; return 0; }
SceUID sceIoOpen(const char *path, int flags, SceMode mode) {
    assert(strcmp(path, CONFIG_FILE) == 0);
    opens++;
    position = 0;
    if (fail_open) return -1;
    if (flags & SCE_O_TRUNC) { disk_size = 0; exists = 1; }
    return exists ? 1 : -1;
}
SceSSize sceIoRead(SceUID fd, void *data, SceSize size) {
    int count = disk_size - position;
    if (count > size) count = size;
    memcpy(data, disk + position, count);
    position += count;
    return count;
}
SceSSize sceIoWrite(SceUID fd, const void *data, SceSize size) {
    writes++;
    if (fail_write) return -1;
    if (write_limit && size > write_limit) size = write_limit;
    assert(position + size < sizeof(disk));
    memcpy(disk + position, data, size);
    position += size;
    disk_size = position;
    return size;
}
int sceIoClose(SceUID fd) { return fail_close ? -1 : 0; }

static void expect_no_storage(const AppConfig *cfg) {
    int old_opens = opens, old_writes = writes, old_mkdirs = mkdirs;
    for (int i = 0; i < 100; ++i) assert(config_save(cfg) == 0);
    assert(opens == old_opens && writes == old_writes && mkdirs == old_mkdirs);
}

int main(void) {
    AppConfig cfg, loaded;
    assert(config_load(&cfg) < 0);
    assert(writes == 0 && mkdirs == 0);
    assert(config_save(&cfg) == 0 && writes == 1);
    expect_no_storage(&cfg);

    /* Cache is seeded on load, so an unchanged reconnect/exit does not write. */
    assert(config_load(&loaded) == 0);
    expect_no_storage(&loaded);
    loaded.server_ip[strlen(loaded.server_ip) + 1] = 'x';
    strcpy(loaded.net_profiles[MAX_NET_PROFILES - 1].server_ip, "unused");
    expect_no_storage(&loaded);

    for (int field = 0; field < 8; ++field) {
        switch (field) {
            case 0: strcpy(cfg.server_ip, "10.4.21.102"); break;
            case 1: cfg.stream_port++; break;
            case 2: cfg.control_port++; break;
            case 3: strcpy(cfg.client_name, "Vita release"); break;
            case 4: cfg.latency_ms = 123; break;
            case 5:
                cfg.net_profile_count = 1;
                strcpy(cfg.net_profiles[0].net_prefix, "10.4.21");
                strcpy(cfg.net_profiles[0].server_ip, "10.4.21.102");
                break;
            case 6: strcpy(cfg.net_profiles[0].server_ip, "10.4.21.103"); break;
            case 7: strcpy(cfg.net_profiles[0].net_prefix, "10.4.22"); break;
        }
        int old_writes = writes;
        assert(config_save(&cfg) == 0 && writes == old_writes + 1);
        assert(config_load(&loaded) == 0 && config_equal(&cfg, &loaded));
        expect_no_storage(&cfg);
    }

    cfg.latency_ms++;
    fail_open = 1;
    assert(config_save(&cfg) < 0);
    fail_open = 0;
    assert(config_save(&cfg) == 0);
    expect_no_storage(&cfg);

    cfg.latency_ms++;
    write_limit = 7;
    assert(config_save(&cfg) == 0); /* Complete short writes. */
    write_limit = 0;
    assert(config_load(&loaded) == 0 && config_equal(&cfg, &loaded));
    expect_no_storage(&cfg);

    cfg.latency_ms++;
    fail_write = 1;
    assert(config_save(&cfg) < 0);
    fail_write = 0;
    int old_writes = writes;
    cfg.latency_ms--; /* Even reverting must repair a failed/truncated save. */
    assert(config_save(&cfg) == 0 && writes == old_writes + 1);
    expect_no_storage(&cfg);

    cfg.latency_ms++;
    fail_close = 1;
    assert(config_save(&cfg) < 0);
    fail_close = 0;
    old_writes = writes;
    assert(config_save(&cfg) == 0 && writes == old_writes + 1);
    expect_no_storage(&cfg);
    puts("config: unchanged loads/reconnects/exits cause no writes; changes, short writes and failed-save retries passed");
    return 0;
}
