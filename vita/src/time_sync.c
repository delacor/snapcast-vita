#include "time_sync.h"
#include <string.h>
#include <stdlib.h>
#include <psp2/kernel/processmgr.h>

/* Use elapsed process time, independent of adjustments to the system RTC. */
static int64_t g_tick_base = 0;

void time_sync_tick_init(void) {
    g_tick_base = (int64_t)sceKernelGetProcessTimeWide();
}

int64_t get_tick_usec(void) {
    return (int64_t)sceKernelGetProcessTimeWide() - g_tick_base;
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

/* --- Time-bounded playback drift history --- */

void sync_filter_reset(SyncFilter *filter) {
    memset(filter, 0, sizeof(*filter));
    filter->first_usec = filter->last_usec = -1;
}

void sync_filter_add(SyncFilter *filter, int64_t now_usec, int64_t age_usec) {
    if (filter->count && (now_usec < filter->last_usec ||
                         now_usec - filter->last_usec > SYNC_WINDOW_USEC))
        sync_filter_reset(filter);
    if (filter->count && now_usec - filter->last_usec < SYNC_SAMPLE_USEC)
        return;
    if (!filter->count) filter->first_usec = now_usec;
    filter->last_usec = now_usec;
    filter->times[filter->write_idx] = now_usec;
    filter->values[filter->write_idx] = age_usec;
    filter->write_idx = (filter->write_idx + 1) % SYNC_FILTER_SIZE;
    if (filter->count < SYNC_FILTER_SIZE) filter->count++;
}

int sync_filter_ready(const SyncFilter *filter, int64_t now_usec) {
    return filter->count > 1 && now_usec >= filter->last_usec &&
           now_usec - filter->last_usec <= SYNC_WINDOW_USEC &&
           now_usec - filter->first_usec >= SYNC_WINDOW_USEC;
}

int64_t sync_filter_median(const SyncFilter *filter, int64_t now_usec) {
    int64_t values[SYNC_FILTER_SIZE];
    int count = 0;
    for (int i = 0; i < filter->count; i++) {
        if (filter->times[i] >= now_usec - SYNC_WINDOW_USEC &&
            filter->times[i] <= now_usec)
            values[count++] = filter->values[i];
    }
    if (!count) return 0;
    qsort(values, count, sizeof(values[0]), cmp_int64);
    return values[count / 2];
}
