#ifndef SNAPVITA_TIME_SYNC_H
#define SNAPVITA_TIME_SYNC_H

#include <stdint.h>

#define TIME_SYNC_BUFFER_SIZE 200

#define AGE_BUFFER_MINI   20
#define AGE_BUFFER_SHORT  100
#define AGE_BUFFER_LONG   500

typedef struct {
    int64_t samples[TIME_SYNC_BUFFER_SIZE];
    int count;
    int write_idx;
    volatile int64_t diff_usec;  /* add to local tick to get server time */
} TimeSync;

typedef struct {
    int64_t *buf;
    int capacity;
    int count;
    int write_idx;
} AgeBuffer;

/* TimeSync: clock offset between Vita and server */
void     time_sync_init(TimeSync *ts);
void     time_sync_update(TimeSync *ts, int64_t c2s_usec, int64_t s2c_usec);
int64_t  time_sync_server_now_usec(const TimeSync *ts);
int      time_sync_valid(const TimeSync *ts);

/* AgeBuffer: rolling buffer with median for chunk age tracking */
int      age_buffer_init(AgeBuffer *ab, int capacity);
void     age_buffer_free(AgeBuffer *ab);
void     age_buffer_add(AgeBuffer *ab, int64_t value);
int64_t  age_buffer_median(const AgeBuffer *ab);
int      age_buffer_full(const AgeBuffer *ab);
void     age_buffer_clear(AgeBuffer *ab);

/* Shared utility */
int64_t  get_tick_usec(void);

#endif /* SNAPVITA_TIME_SYNC_H */
