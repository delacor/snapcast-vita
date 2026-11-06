/* Actual streaming, ring-buffer and grain-rendering code; only Vita OS/audio
 * pacing and Opus decoding are stubbed. No implementation copies in the tests. */
#define main vita_app_main
#include "../src/main.c"
#undef main
#include "../src/audio.c"
#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>

static int64_t now_usec;
static int64_t rtc_offset_usec;
static int stops, starts;
static int create_error, start_error, deletes, output_result;
static int fs_calls, fs_delay_usec, loop_outputs_left;
static int64_t last_output_usec, longest_output_gap_usec;
static int output_extra_usec;
static int output_stall_usec;
static AudioPlayback playback;
static uint32_t input_grain[AUDIO_GRAIN + MAX_FRAME_CORRECTION];
static uint32_t output_grain[AUDIO_GRAIN];

int sceRtcGetCurrentTick(SceRtcTick *tick) { tick->tick = now_usec + rtc_offset_usec; return 0; }
SceUInt64 sceKernelGetProcessTimeWide(void) { return now_usec; }
int sceIoMkdir(const char *path, SceMode mode) { fs_calls++; return 0; }
SceUID sceIoOpen(const char *path, int flags, SceMode mode) {
    fs_calls++;
    now_usec += fs_delay_usec;
    return -1;
}
int sceIoClose(SceUID fd) { fs_calls++; return 0; }
SceSSize sceIoWrite(SceUID fd, const void *data, SceSize size) { fs_calls++; return size; }
int sceAudioOutReleasePort(int port) { return 0; }
int sceAudioOutOpenPort(SceAudioOutPortType type, int len, int rate, SceAudioOutMode mode) { return 1; }
int sceAudioOutSetVolume(int port, SceAudioOutChannelFlag flags, int *vol) { return 0; }
int sceAudioOutOutput(int port, const void *data) {
    if (loop_outputs_left > 0) {
        int64_t gap = now_usec - last_output_usec;
        if (gap > longest_output_gap_usec) longest_output_gap_usec = gap;
        last_output_usec = now_usec;
        now_usec += (AUDIO_GRAIN * 1000000LL + g_audio.sample_rate - 1) / g_audio.sample_rate;
        /* Keep the queue fed while exercising the real playback thread. */
        assert(audio_write(&g_audio, data, AUDIO_GRAIN * g_audio.bytes_per_frame) ==
               AUDIO_GRAIN * g_audio.bytes_per_frame);
        if (--loop_outputs_left == 0) g_audio.running = 0;
    }
    return output_result;
}
SceUID sceKernelCreateThread(const char *name, SceKernelThreadEntry entry, int priority,
                            SceSize stack, SceUInt attr, int affinity, const SceKernelThreadOptParam *opt) {
    return create_error ? create_error : strcmp(name, "audio_out") == 0 ? 1 : 2;
}
int sceKernelStartThread(SceUID id, SceSize size, void *args) {
    if (start_error) return start_error;
    if (id == 1) playback = (AudioPlayback){0};
    starts++;
    return 0;
}
int sceKernelWaitThreadEnd(SceUID id, int *status, SceUInt *timeout) {
    if (id == 1) assert(!g_audio.running);
    else assert(!stream_thread_running);
    stops++;
    return 0;
}
int sceKernelDeleteThread(SceUID id) { deletes++; return 0; }
int opus_decode(OpusDecoder *st, const unsigned char *data, opus_int32 len,
                opus_int16 *pcm, int frame_size, int decode_fec) { abort(); }
const char *opus_strerror(int error) { return "unused"; }
OpusDecoder *opus_decoder_create(opus_int32 rate, int channels, int *error) {
    *error = OPUS_ALLOC_FAIL;
    return NULL;
}
void opus_decoder_destroy(OpusDecoder *decoder) { assert(!decoder); }

/* Feed the real stream worker headers without a live socket. */
static const char *mock_header;
static uint32_t mock_header_size;
static int recv_calls, time_requests;
int net_stream_connect(NetContext *ctx, const char *ip, int port) { return 0; }
void net_stream_disconnect(NetContext *ctx) { }
int net_send_hello(NetContext *ctx, const AppConfig *config) { return 0; }
int net_send_time(NetContext *ctx) { time_requests++; return 0; }
int net_recv_message(NetContext *ctx, BaseMessage *header, char **payload) {
    if (recv_calls++ || !mock_header) return -1;
    memset(header, 0, sizeof(*header));
    header->type = MSG_CODEC_HEADER;
    header->size = mock_header_size;
    *payload = malloc(mock_header_size);
    assert(*payload);
    memcpy(*payload, mock_header, mock_header_size);
    return 0;
}

static void render(void) {
    audio_render_grain(&g_audio, &playback, input_grain, output_grain);
}

static void receive_at(int64_t timestamp, int frames, uint32_t first_sample) {
    uint32_t bytes = frames * g_audio.bytes_per_frame;
    char *payload = malloc(12 + bytes);
    assert(payload);
    int32_t sec = timestamp / 1000000, usec = timestamp % 1000000;
    memcpy(payload, &sec, 4);
    memcpy(payload + 4, &usec, 4);
    memcpy(payload + 8, &bytes, 4);
    for (int i = 0; i < frames; ++i) {
        uint32_t value = first_sample + i;
        memcpy(payload + 12 + i * g_audio.bytes_per_frame, &value, g_audio.bytes_per_frame);
    }
    handle_wire_chunk(payload, 12 + bytes);
    free(payload);
}

static void receive_ms(int ms) {
    receive_at(now_usec + g_time_sync.diff_usec, ms * g_audio.sample_rate / 1000, 1000);
}

static void setup(int buffer_ms) {
    create_error = start_error = output_result = deletes = 0;
    fs_calls = fs_delay_usec = loop_outputs_left = 0;
    now_usec = 0;
    time_sync_tick_init();
    memset(&g_state, 0, sizeof(g_state));
    audio_init(&g_audio);
    assert(audio_configure(&g_audio, 48000, 2, 16, buffer_ms) == 0);
    g_state.buffer_ms = buffer_ms;
    strcpy(g_state.codec, "pcm");
    now_usec = 10000000;
    time_sync_init(&g_time_sync);
    time_sync_update(&g_time_sync, 0, 0);
    reset_sync_buffers();
    audio_start(&g_audio);
    stops = starts = 0;
    output_extra_usec = 0;
    output_stall_usec = 0;
}

static void fill_ms(int ms) {
    int bytes = (ms * g_audio.sample_rate / 1000) * g_audio.bytes_per_frame;
    void *data = calloc(1, bytes);
    assert(data && audio_write(&g_audio, data, bytes) == bytes);
    free(data);
}

/* Drive independent packet and output clocks, including packet bursts. */
static void run_stream(int duration_ms, int chunk_ms) {
    int64_t origin = now_usec, end = origin + (int64_t)duration_ms * 1000;
    int64_t packet = origin, output = origin;
    int grains = 0;
    while (packet < end || output < end) {
        now_usec = packet <= output ? packet : output;
        if (packet <= output && packet < end) {
            receive_ms(chunk_ms);
            packet += chunk_ms * 1000;
        } else if (output < end) {
            render();
            output = origin + (int64_t)(++grains) * AUDIO_GRAIN * 1000000 / g_audio.sample_rate
                     + (int64_t)grains * output_extra_usec
                     + (int64_t)(grains / 500) * output_stall_usec;
        } else break;
    }
    now_usec = end;
}

static void test_playback_pacing_overhead(void) {
    /* A small per-call overhead: 50 us on a 10.67 ms grain, about 0.47%.
     * The old 0.1% correction cap cannot keep up and repeatedly flushes a
     * healthy, nonempty queue when the error reaches 50 ms. */
    const int overheads[] = {-100, 50, 100, 200};
    for (unsigned i = 0; i < sizeof(overheads) / sizeof(overheads[0]); ++i) {
        setup(1000);
        output_extra_usec = overheads[i];
        run_stream(60000, 20);
        assert(stops == 0 && !g_audio.needs_resync && g_audio.prerolled);
        /* Increasing the restart threshold alone would hide accumulating drift. */
        assert(llabs(g_state.last_age_usec) < 50000);
    }
    /* The device log also shows sudden age increases about every 500 grains.
     * Exercise intermittent output stalls as well as continuous clock skew. */
    setup(1000);
    output_stall_usec = 30000;
    run_stream(60000, 20);
    assert(stops == 0 && !g_audio.needs_resync && g_audio.prerolled);
    assert(llabs(g_state.last_age_usec) < 50000);
    puts("60s pacing simulations: no restarts or underruns, final error under 50ms");
}

static void test_scheduling_and_gaps(void) {
    setup(1000);
    time_sync_update(&g_time_sync, 600000000, -600000000);
    receive_ms(20);
    assert(g_audio.scheduled && !g_audio.prerolled);
    assert(g_audio.start_at_usec == now_usec + 1000000);
    now_usec += 999999;
    render();
    assert(!g_audio.prerolled && output_grain[0] == 0);
    now_usec++;
    render();
    assert(g_audio.prerolled && output_grain[0] == 1000);

    /* Original -700 ms case, plus a partly drained 200 ms buffer. */
    for (int target = 1000; target >= 200; target -= 800) {
        setup(target);
        fill_ms(target == 1000 ? 300 : 100);
        g_audio.scheduled = g_audio.prerolled = 1;
        g_hard_sync = 0;
        g_audio.correct_after_x_frames = -17;
        sync_filter_add(&g_sync_filter, now_usec, -700000);
        receive_ms(20);
        assert(stops == 1 && starts == 1);
        assert(g_hard_sync && !g_audio.prerolled && g_audio.scheduled);
        assert(g_audio.correct_after_x_frames == 0 && g_sync_filter.count == 0);
        assert(audio_available(&g_audio) == 3840);
        uint32_t first;
        memcpy(&first, g_audio.ring + (g_audio.read_pos & RING_MASK), 4);
        assert(first == 1000);
        now_usec += 20000;
        run_stream(target + 5000, 20);
        assert(stops == 1 && !g_audio.needs_resync && g_audio.prerolled);
        assert(abs(g_audio.correct_after_x_frames) >= 17 || g_audio.correct_after_x_frames == 0);
    }

    /* The consumer must stop correcting immediately when the queue underruns. */
    setup(200);
    g_audio.scheduled = g_audio.prerolled = 1;
    g_hard_sync = 0;
    g_audio.correct_after_x_frames = -1000;
    render();
    assert(g_audio.needs_resync && g_audio.correct_after_x_frames == 0);
    render();
    assert(g_audio.stat_underruns == 1);
    receive_ms(20);
    assert(stops == 1 && !g_audio.needs_resync);
    assert(g_audio.start_at_usec == now_usec + 200000);

    /* Full source gaps, including repeated YouTube-like resumes. */
    setup(1000);
    run_stream(4000, 20);
    for (int repeat = 0; repeat < 3; repeat++) {
        for (int i = 0; i < 150; i++) { render(); now_usec += 10667; }
        assert(g_audio.needs_resync);
        int previous_stops = stops;
        run_stream(4000, 20);
        assert(stops == previous_stops + 1 && g_audio.prerolled && !g_audio.needs_resync);
    }
    puts("timestamp scheduling, -700ms gap, small-buffer recovery and repeated underruns passed");
}

static void test_delays_and_backlog(void) {
    setup(5000);
    assert(g_audio.buffer_ms == 5000);
    run_stream(8000, 20);
    assert(g_audio.prerolled && !g_audio.needs_resync && stops == 0);
    assert(g_audio.stat_overflows == 0);

    /* A large legal PCM chunk must fit alongside the full target delay. */
    setup(5000);
    run_stream(15000, 3000);
    assert(g_audio.prerolled && !g_audio.needs_resync && stops == 0);
    assert(g_audio.stat_overflows == 0);

    setup(1000);
    run_stream(3000, 20);
    g_state.buffer_ms = 2000;
    run_stream(5000, 20);
    assert(g_audio.buffer_ms == 2000 && stops == 1 && g_audio.prerolled);
    g_state.config.latency_ms = 1000;
    run_stream(4000, 20);
    assert(g_audio.buffer_ms == 1000 && stops == 2 && g_audio.prerolled);

    const char *json = "{\"bufferMs\":1000,\"latency\":123,\"volume\":100,\"muted\":false}";
    char settings[256];
    uint32_t len = strlen(json);
    memcpy(settings, &len, 4);
    memcpy(settings + 4, json, len);
    parse_server_settings(settings, len + 4, &g_state);
    assert(g_state.config.latency_ms == 123);
    run_stream(3000, 20);
    assert(g_audio.buffer_ms == 877 && stops == 3 && g_audio.prerolled);

    setup(1000);
    now_usec = 11500000;
    receive_at(10000000, 960, 1000); /* Entire chunk is overdue. */
    assert(audio_available(&g_audio) == 0 && !g_audio.scheduled);
    receive_at(10490000, 4800, 1000); /* Skip the elapsed 10 ms prefix. */
    assert(audio_available(&g_audio) == (4800 - 480) * 4);
    assert(g_audio.start_at_usec == now_usec);
    now_usec += 5000; /* Consumer wakeup another 5 ms later. */
    render();
    assert(output_grain[0] == 1000 + 480 + 240);
    assert(!g_audio.needs_resync);
    setup(1000);
    receive_at((int64_t)INT_MIN * 1000000, 960, 1000);
    assert(!g_audio.scheduled && audio_available(&g_audio) == 0);
    puts("5s buffer, large PCM packets, live delays, server latency and late trimming passed");
}

static void test_filters_and_correction(void) {
    for (int interval = 20; interval <= 100; interval += 80) {
        setup(1000);
        fill_ms(990);
        g_audio.scheduled = g_audio.prerolled = 1;
        g_hard_sync = 0;
        unsigned char drain[19200];
        for (int elapsed = 0; elapsed <= 2000; elapsed += interval) {
            receive_ms(interval);
            if (elapsed < 2000) assert(g_audio.correct_after_x_frames == 0);
            assert(ring_read(&g_audio, drain, interval * 192) == interval * 192);
            now_usec += interval * 1000;
        }
        assert(g_audio.correct_after_x_frames == -200);
        compute_soft_sync(now_usec, 1000); /* Stop obsolete slowdown across zero. */
        assert(g_audio.correct_after_x_frames == 0);
    }
    SyncFilter filter;
    sync_filter_reset(&filter);
    for (int i = 0; i < 1000; ++i) sync_filter_add(&filter, 0, -10000);
    assert(filter.count == 1 && !sync_filter_ready(&filter, 0));
    for (int t = 20; t <= 4000; t += 20) sync_filter_add(&filter, t * 1000LL, 2000);
    assert(sync_filter_ready(&filter, 4000000) && sync_filter_median(&filter, 4000000) == 2000);
    sync_filter_add(&filter, 10000000, -5000);
    assert(!sync_filter_ready(&filter, 10000000));

    setup(1000);
    fill_ms(1000);
    g_audio.prerolled = g_audio.scheduled = 1;
    g_audio.correct_after_x_frames = 1000;
    uint32_t before = g_audio.read_pos;
    for (int i = 0; i < 10; ++i) render();
    assert(g_audio.read_pos - before == (10 * AUDIO_GRAIN + 5) * 4);
    g_audio.correct_after_x_frames = -1000;
    before = g_audio.read_pos;
    for (int i = 0; i < 10; ++i) render();
    assert(g_audio.read_pos - before == (10 * AUDIO_GRAIN - 5) * 4);
    puts("elapsed-time filtering, 20/100ms chunks and bounded frame correction passed");
}

static void test_supported_formats(void) {
    const int rates[] = {8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000};
    for (unsigned r = 0; r < sizeof(rates) / sizeof(rates[0]); ++r) {
        for (int channels = 1; channels <= 2; ++channels) {
            setup(200);
            assert(audio_configure(&g_audio, rates[r], channels, 16, 200) == 0);
            audio_start(&g_audio);
            stops = starts = 0;
            /* 40 ms has an integral frame count at every supported rate. */
            run_stream(6000, 40);
            assert(g_audio.prerolled && !g_audio.needs_resync && stops == 0);
            assert(g_audio.stat_overflows == 0);
        }
    }
    setup(1000);
    assert(audio_configure(&g_audio, 48000, 2, 24, 1000) < 0);
    assert(audio_configure(&g_audio, 48000, 2, 8, 1000) < 0);
    assert(audio_configure(&g_audio, 96000, 2, 16, 1000) < 0);
    puts("all nine supported rates in mono/stereo and unsupported-format rejection passed");
}

static void test_ring_bounds_and_wrap(void) {
    setup(1000);
    uint32_t *large = malloc(AUDIO_RING_SIZE + 4);
    assert(large);
    for (int i = 0; i < AUDIO_RING_SIZE / 4 + 1; ++i) large[i] = i;
    g_audio.read_pos = g_audio.write_pos = UINT32_MAX - 63;
    assert(audio_write(&g_audio, large, 128) == 128);
    uint32_t out[32];
    assert(ring_read(&g_audio, out, sizeof(out)) == sizeof(out));
    assert(memcmp(out, large, sizeof(out)) == 0 && audio_available(&g_audio) == 0);
    assert(audio_write(&g_audio, large, AUDIO_RING_SIZE + 4) == 0);
    assert(audio_write(&g_audio, large, 3) == 0);
    assert(audio_available(&g_audio) == 0);
    assert(audio_write(&g_audio, large, AUDIO_MAX_CHUNK_SIZE + 4) == 0);
    assert(audio_write(&g_audio, large, AUDIO_MAX_CHUNK_SIZE) == AUDIO_MAX_CHUNK_SIZE);
    assert(audio_write(&g_audio, large + AUDIO_MAX_CHUNK_SIZE / 4, AUDIO_MAX_CHUNK_SIZE) == AUDIO_MAX_CHUNK_SIZE);
    uint32_t read_before = g_audio.read_pos, write_before = g_audio.write_pos;
    assert(audio_write(&g_audio, large, 128) == 0);
    assert(g_audio.read_pos == read_before && g_audio.write_pos == write_before);
    assert(ring_read(&g_audio, out, sizeof(out)) == sizeof(out));
    assert(memcmp(out, large, sizeof(out)) == 0);
    free(large);
    puts("oversized/full/unaligned writes and uint32 counter wrap passed");
}

#define STRESS_FRAMES 1000000
static void *producer(void *unused) {
    for (uint32_t value = 0; value < STRESS_FRAMES; value++)
        while (!audio_write(&g_audio, &value, 4)) sched_yield();
    return NULL;
}
static void *consumer(void *unused) {
    uint32_t expected = 0, frames[137];
    while (expected < STRESS_FRAMES) {
        int bytes = ring_read(&g_audio, frames, sizeof(frames));
        for (int i = 0; i < bytes / 4; i++) assert(frames[i] == expected++);
        if (!bytes) sched_yield();
    }
    return NULL;
}
static void test_concurrent_ring(void) {
    setup(1000);
    g_audio.read_pos = g_audio.write_pos = UINT32_MAX - 4095;
    pthread_t writer, reader;
    assert(pthread_create(&writer, NULL, producer, NULL) == 0);
    assert(pthread_create(&reader, NULL, consumer, NULL) == 0);
    assert(pthread_join(writer, NULL) == 0 && pthread_join(reader, NULL) == 0);
    assert(audio_available(&g_audio) == 0);
    puts("concurrent producer/consumer: 1,000,000 frames in order across counter wrap passed");
}

static void test_monotonic_clock(void) {
    now_usec = 9000000;
    time_sync_tick_init();
    assert(get_tick_usec() == 0);
    now_usec += 123456;
    rtc_offset_usec = 3600000000LL;
    assert(get_tick_usec() == 123456);
    rtc_offset_usec = -3600000000LL;
    assert(get_tick_usec() == 123456);
    now_usec += 1LL << 33;
    assert(get_tick_usec() == (1LL << 33) + 123456);
    rtc_offset_usec = 0;
    puts("elapsed clock ignores RTC changes and survives 32-bit microsecond wrap");
}

static void test_thread_failures(void) {
    setup(1000);
    audio_stop(&g_audio);
    stops = deletes = 0;
    create_error = -10;
    assert(audio_start(&g_audio) == -10);
    assert(!g_audio.running && g_audio.thread_id == -1 && deletes == 0);
    create_error = 0;
    start_error = -11;
    assert(audio_start(&g_audio) == -11);
    assert(!g_audio.running && g_audio.thread_id == -1 && deletes == 1 && stops == 0);
    start_error = 0;
    assert(audio_start(&g_audio) == 0 && g_audio.running);
    output_result = -12;
    AudioContext *ctx = &g_audio;
    assert(audio_thread(sizeof(ctx), &ctx) == 0);
    assert(!g_audio.running && g_audio.output_error == -12);
    g_state.conn_state = CONN_CONNECTED;
    check_audio_output();
    assert(g_state.conn_state == CONN_ERROR && strstr(g_state.conn_error, "Audio output"));
    audio_stop(&g_audio);
    assert(g_audio.thread_id == -1 && stops == 1 && deletes == 2);
    output_result = 0;
    assert(audio_start(&g_audio) == 0 && g_audio.output_error == 0);

    /* Failure during a sync restart must not queue data to a dead consumer. */
    g_audio.needs_resync = 1;
    start_error = -13;
    receive_ms(20);
    assert(!g_audio.running && !g_audio.scheduled && audio_available(&g_audio) == 0);
    assert(g_state.conn_state == CONN_ERROR && strstr(g_state.conn_error, "restart audio"));
    start_error = 0;

    stream_thread_running = 0;
    stream_thread_id = -1;
    stops = deletes = 0;
    create_error = -14;
    g_state.conn_state = CONN_CONNECTED;
    start_streaming();
    assert(!stream_thread_running && stream_thread_id == -1 && deletes == 0);
    assert(g_state.conn_state == CONN_ERROR && strstr(g_state.conn_error, "create stream"));
    create_error = 0;
    start_error = -15;
    g_state.conn_state = CONN_CONNECTED;
    start_streaming();
    assert(!stream_thread_running && stream_thread_id == -1 && deletes == 1 && stops == 0);
    assert(g_state.conn_state == CONN_ERROR && strstr(g_state.conn_error, "start stream"));
    start_error = 0;
    start_streaming();
    assert(stream_thread_running && stream_thread_id >= 0);
    stop_streaming();
    assert(!stream_thread_running && stream_thread_id == -1 && stops == 1 && deletes == 2);
    puts("audio/stream create and start failures, output failure, cleanup and retry passed");
}

/* Same fixed header layouts as server/encoder/{pcm,opus}_encoder.cpp. */
static int make_codec_header(char *out, int opus) {
    const char *codec = opus ? "opus" : "pcm";
    uint32_t len = strlen(codec), bytes = opus ? 12 : 44, rate = 48000;
    uint16_t channels = 2, bits = 16;
    memcpy(out, &len, 4);
    memcpy(out + 4, codec, len);
    memcpy(out + 4 + len, &bytes, 4);
    char *p = out + 8 + len;
    memset(p, 0, bytes);
    if (opus) {
        uint32_t magic = 0x4F505553;
        memcpy(p, &magic, 4);
        memcpy(p + 4, &rate, 4);
        memcpy(p + 8, &bits, 2);
        memcpy(p + 10, &channels, 2);
    } else {
        uint32_t riff_size = 36, fmt_size = 16, byte_rate = 192000;
        uint16_t format = 1, block_align = 4;
        memcpy(p, "RIFF", 4);
        memcpy(p + 4, &riff_size, 4);
        memcpy(p + 8, "WAVEfmt ", 8);
        memcpy(p + 16, &fmt_size, 4);
        memcpy(p + 20, &format, 2);
        memcpy(p + 22, &channels, 2);
        memcpy(p + 24, &rate, 4);
        memcpy(p + 28, &byte_rate, 4);
        memcpy(p + 32, &block_align, 2);
        memcpy(p + 34, &bits, 2);
        memcpy(p + 36, "data", 4);
    }
    return 8 + len + bytes;
}

static void test_codec_headers(void) {
    setup(1000);
    AppState state = {0}, before;
    strcpy(state.codec, "old");
    state.sample_rate = 8000;
    state.bits = 16;
    state.channels = 1;
    char payload[64];
    for (int opus = 0; opus <= 1; ++opus) {
        int size = make_codec_header(payload, opus);
        before = state;
        for (int cut = 0; cut < size; ++cut) {
            /* Use an exactly sized allocation to expose out-of-bounds reads. */
            char *truncated = malloc(cut ? cut : 1);
            assert(truncated);
            memcpy(truncated, payload, cut);
            assert(parse_codec_header(truncated, cut, &state) < 0);
            assert(memcmp(&state, &before, sizeof(state)) == 0);
            free(truncated);
        }
        assert(parse_codec_header(payload, size, &state) == 0);
        assert(strcmp(state.codec, opus ? "opus" : "pcm") == 0);
        assert(state.sample_rate == 48000 && state.bits == 16 && state.channels == 2);
        before = state;
        payload[opus ? 12 : 11] ^= 1; /* Broken codec signature */
        assert(parse_codec_header(payload, size, &state) < 0);
        assert(memcmp(&state, &before, sizeof(state)) == 0);
        for (int field = 0; field < 5; ++field) {
            make_codec_header(payload, opus);
            char *p = payload + (opus ? 12 : 11);
            if (field == 0) {
                uint32_t short_header = 8;
                memcpy(p - 4, &short_header, 4);
            } else if (field == 1) {
                uint32_t rate = 0;
                memcpy(p + (opus ? 4 : 24), &rate, 4);
            } else if (field == 2) {
                uint16_t channels = 3;
                memcpy(p + (opus ? 10 : 22), &channels, 2);
            } else if (field == 3) {
                uint16_t bits = 24;
                memcpy(p + (opus ? 8 : 34), &bits, 2);
            } else {
                payload[5] = '\0';
            }
            assert(parse_codec_header(payload, size, &state) < 0);
            assert(memcmp(&state, &before, sizeof(state)) == 0);
        }
    }

    /* Exercise actual worker failure propagation for bad headers, a failed
     * decoder allocation, and failed audio startup after a valid PCM header. */
    for (int scenario = 0; scenario < 4; ++scenario) {
        setup(1000);
        mock_header_size = make_codec_header(payload, scenario == 1);
        mock_header = payload;
        if (scenario == 0) mock_header_size--;
        if (scenario == 2) start_error = -16;
        recv_calls = time_requests = 0;
        stream_thread_running = 1;
        g_state.conn_state = CONN_CONNECTED;
        assert(stream_thread_func(0, NULL) == 0);
        assert(!stream_thread_running && g_state.conn_state == CONN_ERROR);
        if (scenario < 3) {
            assert(recv_calls == 1 && time_requests == 0);
            assert(strstr(g_state.conn_error, scenario == 0 ? "codec header" :
                          scenario == 1 ? "audio format" : "start audio"));
        } else {
            assert(recv_calls == 2 && time_requests == 1 && g_audio.running);
            assert(strcmp(g_state.codec, "pcm") == 0);
        }
        stop_streaming();
        assert(!g_audio.running && g_audio.thread_id == -1);
    }
    mock_header = NULL;
    puts("PCM/Opus validation, every truncated header length and worker failure propagation passed");
}

static void test_audio_thread_does_not_block_on_logging(void) {
    setup(1000);
    fill_ms(1000);
    g_audio.prerolled = g_audio.scheduled = 1;
    fs_calls = 0;
    fs_delay_usec = 100000; /* Slow storage operation, matching a short dropout. */
    loop_outputs_left = 3000; /* 32 seconds, across six former log intervals. */
    longest_output_gap_usec = 0;
    last_output_usec = now_usec;
    AudioContext *ctx = &g_audio;
    assert(audio_thread(sizeof(ctx), &ctx) == 0);
    fprintf(stderr, "audio loop: filesystem calls=%d, longest output gap=%lld us\n",
            fs_calls, (long long)longest_output_gap_usec);
    assert(loop_outputs_left == 0 && !g_audio.needs_resync);
    assert(fs_calls == 0 && longest_output_gap_usec <= 10667);
    fs_delay_usec = 0;
    audio_stop(&g_audio);
}

int main(void) {
#if !SNAPVITA_FILE_LOGGING && !SNAPVITA_AUDIO_DUMP
    setup(1000);
    assert(fs_calls == 0); /* Includes audio initialization and thread startup. */
    run_stream(60000, 20);
    assert(fs_calls == 0);
    audio_stop(&g_audio);
    pcm_dump_init();
    pcm_dump_write("test", 4);
    main_log("release log test\n");
    audio_log("release audio log test\n");
    assert(fs_calls == 0);
    puts("release initialization, 60s playback, shutdown and diagnostics: zero filesystem calls");
#endif
    test_audio_thread_does_not_block_on_logging();
    test_monotonic_clock();
    test_thread_failures();
    test_codec_headers();
    test_ring_bounds_and_wrap();
    test_playback_pacing_overhead();
    test_scheduling_and_gaps();
    test_delays_and_backlog();
    test_filters_and_correction();
    test_supported_formats();
    test_concurrent_ring();
    puts("all sync regression tests passed");
    return 0;
}
