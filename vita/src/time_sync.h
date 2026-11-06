#ifndef SNAPVITA_TIME_SYNC_H
#define SNAPVITA_TIME_SYNC_H

#include <stdint.h>

#define TIME_SYNC_BUFFER_SIZE 200

#define SYNC_WINDOW_USEC 2000000
#define SYNC_SAMPLE_USEC 20000
#define SYNC_FILTER_SIZE (SYNC_WINDOW_USEC / SYNC_SAMPLE_USEC + 1)

typedef struct {
    int64_t samples[TIME_SYNC_BUFFER_SIZE];
    int count;
    int write_idx;
    volatile int64_t diff_usec;  /* add to local tick to get server time */
} TimeSync;

/* Time-bounded drift history. Sampling is throttled to bound memory even
 * when the network delivers many queued chunks in one burst. */
typedef struct {
    int64_t values[SYNC_FILTER_SIZE];
    int64_t times[SYNC_FILTER_SIZE];
    int64_t first_usec;
    int64_t last_usec;
    int count;
    int write_idx;
} SyncFilter;

/* TimeSync: clock offset between Vita and server */
void     time_sync_init(TimeSync *ts);
void     time_sync_update(TimeSync *ts, int64_t c2s_usec, int64_t s2c_usec);
int64_t  time_sync_server_now_usec(const TimeSync *ts);
int      time_sync_valid(const TimeSync *ts);

/* Call once at app start to establish the monotonic process-time base.
   All subsequent get_tick_usec() calls return µs since this call.
   This keeps the sec/usec values small (no int32_t overflow). */
void     time_sync_tick_init(void);

void    sync_filter_reset(SyncFilter *filter);
void    sync_filter_add(SyncFilter *filter, int64_t now_usec, int64_t age_usec);
int     sync_filter_ready(const SyncFilter *filter, int64_t now_usec);
int64_t sync_filter_median(const SyncFilter *filter, int64_t now_usec);

/* Shared utility */
int64_t  get_tick_usec(void);

#endif /* SNAPVITA_TIME_SYNC_H */
