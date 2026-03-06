#include "time_sync.h"
#include <string.h>
#include <stdlib.h>
#include <psp2/rtc.h>

/* Base tick set once at app start so all relative values fit in int32_t */
static int64_t g_tick_base = 0;

void time_sync_tick_init(void) {
    SceRtcTick tick;
    sceRtcGetCurrentTick(&tick);
    g_tick_base = (int64_t)tick.tick;
}

int64_t get_tick_usec(void) {
    SceRtcTick tick;
    sceRtcGetCurrentTick(&tick);
    return (int64_t)tick.tick - g_tick_base;
}

static int cmp_int64(const void *a, const void *b) {
    int64_t va = *(const int64_t *)a;
    int64_t vb = *(const int64_t *)b;
    return (va > vb) - (va < vb);
}

static int64_t compute_median(const int64_t *arr, int count) {
    if (count <= 0) return 0;
    int64_t *tmp = (int64_t *)malloc(count * sizeof(int64_t));
    if (!tmp) return arr[0];
    memcpy(tmp, arr, count * sizeof(int64_t));
    qsort(tmp, count, sizeof(int64_t), cmp_int64);
    int64_t med = tmp[count / 2];
    free(tmp);
    return med;
}

/* --- TimeSync --- */

void time_sync_init(TimeSync *ts) {
    memset(ts, 0, sizeof(*ts));
}

void time_sync_update(TimeSync *ts, int64_t c2s_usec, int64_t s2c_usec) {
    /* NTP clock-offset formula: diff = (c2s - s2c) / 2
     *   c2s = server_received - client_sent  (one-way latency client->server)
     *   s2c = client_received - server_sent  (one-way latency server->client)
     * For symmetric transit: diff = clock_offset = server_time - client_time  */
    int64_t diff = (c2s_usec - s2c_usec) / 2;

    ts->samples[ts->write_idx] = diff;
    ts->write_idx = (ts->write_idx + 1) % TIME_SYNC_BUFFER_SIZE;
    if (ts->count < TIME_SYNC_BUFFER_SIZE)
        ts->count++;

    ts->diff_usec = compute_median(ts->samples, ts->count);
}

int64_t time_sync_server_now_usec(const TimeSync *ts) {
    return get_tick_usec() + ts->diff_usec;
}

int time_sync_valid(const TimeSync *ts) {
    return ts->count > 0;
}

/* --- AgeBuffer --- */

int age_buffer_init(AgeBuffer *ab, int capacity) {
    memset(ab, 0, sizeof(*ab));
    ab->buf = (int64_t *)calloc(capacity, sizeof(int64_t));
    if (!ab->buf) return -1;
    ab->capacity = capacity;
    return 0;
}

void age_buffer_free(AgeBuffer *ab) {
    free(ab->buf);
    memset(ab, 0, sizeof(*ab));
}

void age_buffer_add(AgeBuffer *ab, int64_t value) {
    if (!ab->buf) return;
    ab->buf[ab->write_idx] = value;
    ab->write_idx = (ab->write_idx + 1) % ab->capacity;
    if (ab->count < ab->capacity)
        ab->count++;
}

int64_t age_buffer_median(const AgeBuffer *ab) {
    return compute_median(ab->buf, ab->count);
}

int age_buffer_full(const AgeBuffer *ab) {
    return ab->count >= ab->capacity;
}

void age_buffer_clear(AgeBuffer *ab) {
    ab->count = 0;
    ab->write_idx = 0;
}
