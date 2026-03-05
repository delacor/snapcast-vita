#include "gui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vita2d.h>
#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <psp2/ime_dialog.h>
#include <psp2/common_dialog.h>

/* --- Color palette (dark theme) --- */
#define COL_BG         RGBA8(18, 18, 32, 255)
#define COL_SURFACE    RGBA8(28, 32, 58, 255)
#define COL_CARD       RGBA8(36, 42, 74, 255)
#define COL_CARD_HL    RGBA8(48, 56, 96, 255)
#define COL_PRIMARY    RGBA8(66, 135, 245, 255)
#define COL_ACCENT     RGBA8(233, 69, 96, 255)
#define COL_WHITE      RGBA8(240, 240, 245, 255)
#define COL_GRAY       RGBA8(160, 160, 175, 255)
#define COL_DIM        RGBA8(90, 90, 110, 255)
#define COL_GREEN      RGBA8(76, 175, 80, 255)
#define COL_ORANGE     RGBA8(255, 165, 0, 255)
#define COL_RED        RGBA8(244, 67, 54, 255)
#define COL_TAB_BG     RGBA8(24, 26, 48, 255)
#define COL_TAB_ACT    RGBA8(233, 69, 96, 255)
#define COL_SLIDER_BG  RGBA8(50, 54, 86, 255)
#define COL_SLIDER_FG  RGBA8(66, 135, 245, 255)
#define COL_ART_BG     RGBA8(44, 50, 88, 255)
#define COL_BTN        RGBA8(52, 58, 100, 255)
#define COL_BTN_HL     RGBA8(72, 80, 130, 255)

static vita2d_pgf *font = NULL;
static SceCtrlData pad, pad_old;
static SceTouchData touch;

#define BTN_PRESSED(b) ((pad.buttons & (b)) && !(pad_old.buttons & (b)))

/* IME state */
static int ime_open = 0;
static int ime_field_id = 0;
static SceWChar16 ime_title_buf[64];
static SceWChar16 ime_initial_buf[256];
static SceWChar16 ime_result_buf[256];

static void a2w(const char *s, SceWChar16 *d, int max) {
    int i = 0;
    while (s[i] && i < max - 1) { d[i] = (SceWChar16)s[i]; i++; }
    d[i] = 0;
}

static void w2a(const SceWChar16 *s, char *d, int max) {
    int i = 0;
    while (s[i] && i < max - 1) { d[i] = (char)s[i]; i++; }
    d[i] = 0;
}

static const char *tab_names[SCREEN_COUNT] = {
    "Connect", "Player", "Devices", "Groups", "Settings"
};

int gui_init(void) {
    vita2d_init();
    vita2d_set_clear_color(COL_BG);

    font = vita2d_load_default_pgf();
    if (!font) return -1;

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);

    memset(&pad, 0, sizeof(pad));
    memset(&pad_old, 0, sizeof(pad_old));
    return 0;
}

void gui_fini(void) {
    if (font) vita2d_free_pgf(font);
    vita2d_fini();
}

/* --- Drawing helpers --- */

static void draw_text(int x, int y, unsigned int color, float scale, const char *text) {
    vita2d_pgf_draw_text(font, x, y, color, scale, text);
}

static int text_width(float scale, const char *text) {
    return vita2d_pgf_text_width(font, scale, text);
}

static void draw_rect(int x, int y, int w, int h, unsigned int color) {
    vita2d_draw_rectangle((float)x, (float)y, (float)w, (float)h, color);
}

static void draw_card(int x, int y, int w, int h, int selected) {
    draw_rect(x, y, w, h, selected ? COL_CARD_HL : COL_CARD);
}

static void draw_slider(int x, int y, int w, int h, float value, unsigned int fg, unsigned int bg) {
    draw_rect(x, y, w, h, bg);
    int filled = (int)(w * value);
    if (filled > 0) draw_rect(x, y, filled, h, fg);
}

static void draw_button(int x, int y, int w, int h, const char *label, int highlight) {
    draw_rect(x, y, w, h, highlight ? COL_BTN_HL : COL_BTN);
    int tw = text_width(0.9f, label);
    draw_text(x + (w - tw) / 2, y + h / 2 + 5, COL_WHITE, 0.9f, label);
}

/* --- Top bar --- */

static void draw_top_bar(AppState *state) {
    draw_rect(0, 0, SCREEN_W, TOP_BAR_H, COL_TAB_BG);

    draw_text(16, 30, COL_ACCENT, 1.1f, "SNAPCAST");

    int tab_x = 180;
    for (int i = 0; i < SCREEN_COUNT; i++) {
        int tw = text_width(0.85f, tab_names[i]);
        int pad_w = tw + 24;
        unsigned int col = (i == state->current_screen) ? COL_WHITE : COL_DIM;
        draw_text(tab_x + 12, 29, col, 0.85f, tab_names[i]);
        if (i == state->current_screen)
            draw_rect(tab_x + 6, TOP_BAR_H - 3, pad_w - 12, 3, COL_TAB_ACT);
        tab_x += pad_w + 8;
    }

    if (state->conn_state == CONN_CONNECTED) {
        draw_rect(SCREEN_W - 24, 16, 10, 10, COL_GREEN);
    } else if (state->conn_state == CONN_RECONNECTING) {
        draw_rect(SCREEN_W - 24, 16, 10, 10, COL_ORANGE);
    } else {
        draw_rect(SCREEN_W - 24, 16, 10, 10, COL_RED);
    }
}

/* --- Bottom bar --- */

static void draw_bottom_bar(AppState *state) {
    int y = SCREEN_H - BOTTOM_BAR_H;
    draw_rect(0, y, SCREEN_W, BOTTOM_BAR_H, COL_TAB_BG);

    const char *hints = "";
    switch (state->current_screen) {
        case SCREEN_CONNECT:  hints = "X:Select  START:Connect  L/R:Tab"; break;
        case SCREEN_PLAYER:   hints = "X:Play/Pause  SQ:Stop  </>:Vol  ^v:Prev/Next  L/R:Tab"; break;
        case SCREEN_DEVICES:  hints = "UP/DN:Select  TRI:Mute  </>:Vol  L/R:Tab"; break;
        case SCREEN_GROUPS:   hints = "UP/DN:Select  X:Action  L/R:Tab"; break;
        case SCREEN_SETTINGS: hints = "X:Edit  </>:Adjust  L/R:Tab"; break;
        default: break;
    }
    draw_text(16, y + 24, COL_DIM, 0.75f, hints);
}

/* --- Connect screen --- */

static void draw_connect_screen(AppState *state) {
    int cx = SCREEN_W / 2;
    int y = CONTENT_Y + 40;

    draw_text(cx - text_width(1.2f, "Snapcast for PS Vita") / 2, y, COL_WHITE, 1.2f,
              "Snapcast for PS Vita");
    y += 16;
    draw_text(cx - text_width(0.8f, "Multi-room audio client") / 2, y + 20, COL_GRAY, 0.8f,
              "Multi-room audio client");
    y += 60;

    int field_x = cx - 180;
    int field_w = 360;
    int field_h = 36;

    draw_text(field_x, y + 5, COL_GRAY, 0.85f, "Server IP:");
    draw_card(field_x + 130, y - 10, field_w - 130, field_h, state->selected_item == 0);
    draw_text(field_x + 140, y + 12, COL_WHITE, 0.9f, state->config.server_ip);
    y += 50;

    draw_text(field_x, y + 5, COL_GRAY, 0.85f, "Port:");
    draw_card(field_x + 130, y - 10, field_w - 130, field_h, state->selected_item == 1);
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", state->config.stream_port);
    draw_text(field_x + 140, y + 12, COL_WHITE, 0.9f, port_str);
    y += 60;

    const char *btn_label = "CONNECT";
    if (state->conn_state == CONN_CONNECTING) btn_label = "CONNECTING...";
    else if (state->conn_state == CONN_CONNECTED) btn_label = "DISCONNECT";
    else if (state->conn_state == CONN_RECONNECTING) btn_label = "CANCEL";

    draw_button(cx - 100, y, 200, 40, btn_label, state->selected_item == 2);
    y += 60;

    char status_line[MAX_STR_LEN + 64];
    unsigned int status_col = COL_GRAY;

    switch (state->conn_state) {
        case CONN_CONNECTED:
            snprintf(status_line, sizeof(status_line), "Status: Connected");
            status_col = COL_GREEN;
            break;
        case CONN_CONNECTING:
            snprintf(status_line, sizeof(status_line), "Status: Connecting...");
            status_col = COL_ORANGE;
            break;
        case CONN_RECONNECTING: {
            int rem = state->reconnect_delay_frames - state->reconnect_timer;
            int rem_secs = (rem + 59) / 60;
            if (rem_secs < 0) rem_secs = 0;
            snprintf(status_line, sizeof(status_line),
                     "Status: Reconnecting in %ds (attempt %d)",
                     rem_secs, state->reconnect_attempts);
            status_col = COL_ORANGE;
            break;
        }
        case CONN_ERROR:
            snprintf(status_line, sizeof(status_line), "Status: %s", state->conn_error);
            status_col = COL_RED;
            break;
        default:
            snprintf(status_line, sizeof(status_line), "Status: Disconnected");
            break;
    }
    draw_text(cx - text_width(0.85f, status_line) / 2, y, status_col, 0.85f, status_line);

    if (state->conn_state == CONN_CONNECTED && state->server.host_name[0]) {
        y += 25;
        char srv_info[192];
        snprintf(srv_info, sizeof(srv_info), "Server: %s v%s",
                 state->server.host_name, state->server.version);
        draw_text(cx - text_width(0.75f, srv_info) / 2, y, COL_DIM, 0.75f, srv_info);

        y += 22;
        if (state->codec[0]) {
            char fmt_info[128];
            snprintf(fmt_info, sizeof(fmt_info), "Audio: %s  %dHz / %dbit / %dch",
                     state->codec, state->sample_rate, state->bits, state->channels);
            draw_text(cx - text_width(0.75f, fmt_info) / 2, y,
                      COL_PRIMARY, 0.75f, fmt_info);
        }
    }

    /* Debug hints */
    draw_text(cx - 160, SCREEN_H - BOTTOM_BAR_H - 32, COL_DIM, 0.60f,
              "Debug log: ux0:data/snapcast/debug.log");
    draw_text(cx - 160, SCREEN_H - BOTTOM_BAR_H - 16, COL_DIM, 0.60f,
              "PCM dump:  ux0:data/snapcast/audio_dump.raw  (5s, sox -r 48000 -e signed-integer -b 16 -c 2 -L)");
}

/* --- Player screen --- */

static SnapStream *find_our_stream(AppState *state) {
    for (int g = 0; g < state->server.group_count; g++) {
        SnapGroup *grp = &state->server.groups[g];
        for (int c = 0; c < grp->client_count; c++) {
            if (strcmp(grp->clients[c].id, state->our_client_id) == 0) {
                for (int s = 0; s < state->server.stream_count; s++) {
                    if (strcmp(state->server.streams[s].id, grp->stream_id) == 0)
                        return &state->server.streams[s];
                }
            }
        }
    }
    return (state->server.stream_count > 0) ? &state->server.streams[0] : NULL;
}

static void draw_player_screen(AppState *state, AudioContext *audio) {
    if (state->conn_state != CONN_CONNECTED) {
        draw_text(SCREEN_W / 2 - 80, SCREEN_H / 2, COL_GRAY, 1.0f, "Not connected");
        return;
    }

    SnapStream *stream = find_our_stream(state);
    StreamMetadata *meta = stream ? &stream->properties.metadata : NULL;
    int y = CONTENT_Y + 20;

    /* Cover art area (left side) */
    int art_x = 40, art_y = y, art_size = 180;
    draw_rect(art_x, art_y, art_size, art_size, COL_ART_BG);
    if (meta && meta->title[0]) {
        draw_text(art_x + art_size / 2 - 10, art_y + art_size / 2 + 8, COL_DIM, 2.0f, "~");
    } else {
        draw_text(art_x + art_size / 2 - 10, art_y + art_size / 2 + 8, COL_DIM, 2.0f, "~");
    }

    /* Track info (right side) */
    int info_x = art_x + art_size + 30;
    int info_y = art_y + 10;

    if (meta && meta->title[0]) {
        draw_text(info_x, info_y, COL_WHITE, 1.0f, meta->title);
        info_y += 28;
        if (meta->artist[0]) {
            draw_text(info_x, info_y, COL_GRAY, 0.85f, meta->artist);
            info_y += 24;
        }
        if (meta->album[0]) {
            draw_text(info_x, info_y, COL_DIM, 0.8f, meta->album);
            info_y += 24;
        }
    } else {
        draw_text(info_x, info_y, COL_GRAY, 0.9f, "No track playing");
        info_y += 28;
    }

    info_y += 8;
    if (stream) {
        char stream_info[128];
        snprintf(stream_info, sizeof(stream_info), "Stream: %s (%s)",
                 stream->name[0] ? stream->name : stream->id, stream->status);
        draw_text(info_x, info_y, COL_DIM, 0.75f, stream_info);
        info_y += 22;

        const char *ps = stream->properties.playback_status;
        if (ps[0]) {
            char pb_info[64];
            snprintf(pb_info, sizeof(pb_info), "Status: %s", ps);
            unsigned int pb_col = COL_DIM;
            if (strcmp(ps, "playing") == 0 || strcmp(ps, "Playing") == 0)
                pb_col = COL_GREEN;
            draw_text(info_x, info_y, pb_col, 0.75f, pb_info);
        }
    }

    /* Transport controls */
    y = art_y + art_size + 30;
    int btn_w = 80, btn_h = 36, btn_gap = 16;
    int btns_w = 3 * btn_w + 2 * btn_gap;
    int btn_x = 40;

    int can_prev = stream && stream->properties.can_go_previous;
    int can_play = stream && (stream->properties.can_play || stream->properties.can_pause);
    int can_next = stream && stream->properties.can_go_next;

    draw_button(btn_x, y, btn_w, btn_h, "|<<", can_prev);
    draw_button(btn_x + btn_w + btn_gap, y, btn_w, btn_h, "> / ||", can_play);
    draw_button(btn_x + 2 * (btn_w + btn_gap), y, btn_w, btn_h, ">>|", can_next);

    /* Volume slider */
    int vol_x = btn_x + btns_w + 60;
    int vol_w = SCREEN_W - vol_x - 50;
    int vol_y = y + 8;

    draw_text(vol_x, vol_y - 2, COL_GRAY, 0.75f, "Volume:");
    vol_x += 80;
    vol_w -= 80;

    float vol_f = state->volume_percent / 100.0f;
    draw_slider(vol_x, vol_y, vol_w, 16, vol_f, COL_SLIDER_FG, COL_SLIDER_BG);

    char vol_str[16];
    snprintf(vol_str, sizeof(vol_str), "%d%%", state->volume_percent);
    draw_text(vol_x + vol_w + 8, vol_y + 4, state->volume_muted ? COL_RED : COL_WHITE,
              0.75f, vol_str);
    if (state->volume_muted)
        draw_text(vol_x + vol_w + 50, vol_y + 4, COL_RED, 0.75f, "MUTED");

    /* Progress bar */
    if (meta && meta->duration > 0) {
        y += 56;
        float progress = (meta->duration > 0) ? (float)(meta->position / meta->duration) : 0;
        if (progress > 1.0f) progress = 1.0f;

        char pos_str[32], dur_str[32];
        int p_min = (int)meta->position / 60, p_sec = (int)meta->position % 60;
        int d_min = (int)meta->duration / 60, d_sec = (int)meta->duration % 60;
        snprintf(pos_str, sizeof(pos_str), "%d:%02d", p_min, p_sec);
        snprintf(dur_str, sizeof(dur_str), "%d:%02d", d_min, d_sec);

        draw_text(40, y + 6, COL_GRAY, 0.75f, pos_str);
        int bar_x = 100, bar_w = SCREEN_W - 200;
        draw_slider(bar_x, y, bar_w, 8, progress, COL_ACCENT, COL_SLIDER_BG);
        draw_text(SCREEN_W - 80, y + 6, COL_GRAY, 0.75f, dur_str);
    }

    /* Audio info footer + buffer fill bar */
    y = SCREEN_H - BOTTOM_BAR_H - 52;
    if (state->codec[0]) {
        char audio_info[128];
        snprintf(audio_info, sizeof(audio_info),
                 "Codec: %s  %dHz/%dbit/%dch  srv-buf: %dms",
                 state->codec, state->sample_rate, state->bits,
                 state->channels, state->buffer_ms);
        draw_text(40, y, COL_DIM, 0.7f, audio_info);
    }
    y += 20;
    /* Ring buffer fill bar */
    if (audio) {
        float fill = audio_fill_ratio(audio);
        int bar_x = 40, bar_w = 500, bar_h = 8;
        draw_slider(bar_x, y, bar_w, bar_h, fill, COL_PRIMARY, COL_SLIDER_BG);

        char fill_str[64];
        int fill_kb = (int)(fill * AUDIO_RING_SIZE / 1024);
        snprintf(fill_str, sizeof(fill_str), "ring: %dKB/%dKB  U:%d O:%d",
                 fill_kb, AUDIO_RING_SIZE / 1024,
                 audio->stat_underruns, audio->stat_overflows);
        draw_text(bar_x + bar_w + 12, y + 6, COL_DIM, 0.65f, fill_str);

        /* Color the bar red if critically low (likely about to underrun) */
        if (fill < 0.1f && audio->prerolled) {
            draw_slider(bar_x, y, (int)(bar_w * fill), bar_h, 1.0f, COL_RED, COL_SLIDER_BG);
        }
    }
}

/* --- Devices screen --- */

static void draw_devices_screen(AppState *state) {
    if (state->conn_state != CONN_CONNECTED) {
        draw_text(SCREEN_W / 2 - 80, SCREEN_H / 2, COL_GRAY, 1.0f, "Not connected");
        return;
    }

    int y = CONTENT_Y + 10;
    draw_text(40, y + 16, COL_WHITE, 1.0f, "Devices");
    y += 36;

    int item_idx = 0;
    for (int g = 0; g < state->server.group_count; g++) {
        SnapGroup *grp = &state->server.groups[g];
        for (int c = 0; c < grp->client_count; c++) {
            SnapClient *cli = &grp->clients[c];
            int selected = (item_idx == state->selected_item);
            int card_y = y;
            int card_h = 52;

            if (card_y + card_h > SCREEN_H - BOTTOM_BAR_H - 10) break;

            draw_card(30, card_y, SCREEN_W - 60, card_h, selected);

            unsigned int dot_col = cli->connected ? COL_GREEN : COL_RED;
            vita2d_draw_fill_circle(54, card_y + card_h / 2, 6, dot_col);

            const char *display_name = cli->name[0] ? cli->name : cli->host_name;
            int is_us = (strcmp(cli->id, state->our_client_id) == 0);
            draw_text(72, card_y + 20, COL_WHITE, 0.9f, display_name);

            if (is_us)
                draw_text(72 + text_width(0.9f, display_name) + 10, card_y + 20,
                          COL_ACCENT, 0.7f, "(this device)");

            char ip_str[64];
            snprintf(ip_str, sizeof(ip_str), "%s", cli->ip);
            draw_text(72, card_y + 40, COL_DIM, 0.7f, ip_str);

            /* Volume on the right */
            int vol_bar_x = SCREEN_W - 260;
            int vol_bar_w = 140;
            float vol_f = cli->volume.percent / 100.0f;
            draw_slider(vol_bar_x, card_y + 18, vol_bar_w, 12, vol_f,
                        COL_SLIDER_FG, COL_SLIDER_BG);

            char vol_text[32];
            snprintf(vol_text, sizeof(vol_text), "%d%%", cli->volume.percent);
            draw_text(vol_bar_x + vol_bar_w + 8, card_y + 24,
                      cli->volume.muted ? COL_RED : COL_WHITE, 0.75f, vol_text);

            if (cli->volume.muted)
                draw_text(SCREEN_W - 62, card_y + 24, COL_RED, 0.7f, "M");

            y += card_h + 4;
            item_idx++;
        }
    }

    if (item_idx == 0) {
        draw_text(SCREEN_W / 2 - 60, SCREEN_H / 2, COL_GRAY, 0.9f, "No devices found");
    }
}

/* --- Groups screen --- */

static void draw_groups_screen(AppState *state) {
    if (state->conn_state != CONN_CONNECTED) {
        draw_text(SCREEN_W / 2 - 80, SCREEN_H / 2, COL_GRAY, 1.0f, "Not connected");
        return;
    }

    int y = CONTENT_Y + 10;
    draw_text(40, y + 16, COL_WHITE, 1.0f, "Groups");
    y += 36;

    for (int g = 0; g < state->server.group_count; g++) {
        SnapGroup *grp = &state->server.groups[g];
        int selected = (g == state->selected_group);

        if (y + 30 > SCREEN_H - BOTTOM_BAR_H - 10) break;

        draw_rect(30, y, SCREEN_W - 60, 28, selected ? COL_SURFACE : COL_TAB_BG);

        const char *gname = grp->name[0] ? grp->name : "Unnamed Group";
        draw_text(46, y + 20, selected ? COL_WHITE : COL_GRAY, 0.9f, gname);

        char stream_label[128];
        snprintf(stream_label, sizeof(stream_label), "Stream: %s%s",
                 grp->stream_id, grp->muted ? "  [MUTED]" : "");
        draw_text(SCREEN_W / 2 + 40, y + 20, COL_DIM, 0.75f, stream_label);
        y += 32;

        for (int c = 0; c < grp->client_count; c++) {
            SnapClient *cli = &grp->clients[c];
            if (y + 24 > SCREEN_H - BOTTOM_BAR_H - 10) break;

            unsigned int dot_col = cli->connected ? COL_GREEN : COL_RED;
            vita2d_draw_fill_circle(68, y + 10, 4, dot_col);

            const char *cname = cli->name[0] ? cli->name : cli->host_name;
            int is_us = (strcmp(cli->id, state->our_client_id) == 0);
            draw_text(82, y + 16, is_us ? COL_ACCENT : COL_GRAY, 0.8f, cname);

            char vol_str[16];
            snprintf(vol_str, sizeof(vol_str), "%d%%", cli->volume.percent);
            draw_text(SCREEN_W - 100, y + 16, COL_DIM, 0.7f, vol_str);
            y += 24;
        }
        y += 10;
    }

    if (state->server.group_count == 0)
        draw_text(SCREEN_W / 2 - 60, SCREEN_H / 2, COL_GRAY, 0.9f, "No groups found");

    /* Stream selector at bottom */
    if (state->server.stream_count > 0) {
        int sy = SCREEN_H - BOTTOM_BAR_H - 50;
        draw_rect(30, sy, SCREEN_W - 60, 40, COL_SURFACE);
        draw_text(42, sy + 26, COL_GRAY, 0.75f, "Available streams:");
        int sx = 210;
        for (int s = 0; s < state->server.stream_count; s++) {
            SnapStream *st = &state->server.streams[s];
            unsigned int col = (strcmp(st->status, "playing") == 0) ? COL_GREEN : COL_DIM;
            draw_text(sx, sy + 26, col, 0.75f, st->id);
            sx += text_width(0.75f, st->id) + 20;
        }
    }
}

/* --- Settings screen --- */

static void draw_settings_screen(AppState *state, AudioContext *audio) {
    int y = CONTENT_Y + 20;
    int lx = 60;

    draw_text(lx, y + 16, COL_WHITE, 1.0f, "Settings");
    y += 44;

    /* Client name */
    draw_text(lx, y + 14, COL_GRAY, 0.85f, "Client Name:");
    draw_card(lx + 170, y - 4, 300, 34, state->selected_item == 0);
    draw_text(lx + 180, y + 14, COL_WHITE, 0.9f, state->config.client_name);
    y += 50;

    /* Latency */
    draw_text(lx, y + 14, COL_GRAY, 0.85f, "Latency:");
    draw_card(lx + 170, y - 4, 200, 34, state->selected_item == 1);
    char lat_str[32];
    snprintf(lat_str, sizeof(lat_str), "%d ms", state->config.latency_ms);
    draw_text(lx + 180, y + 14, COL_WHITE, 0.9f, lat_str);
    draw_text(lx + 380, y + 14, COL_DIM, 0.7f, "(</>  to adjust)");
    y += 60;

    draw_rect(lx, y, SCREEN_W - 2 * lx, 1, COL_DIM);
    y += 16;

    /* Audio info */
    if (state->conn_state == CONN_CONNECTED) {
        draw_text(lx, y + 14, COL_GRAY, 0.85f, "Audio:");
        char audio_str[128];
        snprintf(audio_str, sizeof(audio_str), "%d Hz / %d bit / %d ch",
                 state->sample_rate, state->bits, state->channels);
        draw_text(lx + 170, y + 14, COL_WHITE, 0.85f, audio_str);
        y += 30;

        draw_text(lx, y + 14, COL_GRAY, 0.85f, "Codec:");
        draw_text(lx + 170, y + 14, COL_WHITE, 0.85f, state->codec);
        y += 30;

        draw_text(lx, y + 14, COL_GRAY, 0.85f, "Srv buf:");
        char buf_str[32];
        snprintf(buf_str, sizeof(buf_str), "%d ms", state->buffer_ms);
        draw_text(lx + 170, y + 14, COL_WHITE, 0.85f, buf_str);
        y += 30;

        draw_text(lx, y + 14, COL_GRAY, 0.85f, "Server:");
        char srv_str[192];
        snprintf(srv_str, sizeof(srv_str), "%s v%s",
                 state->server.host_name, state->server.version);
        draw_text(lx + 170, y + 14, COL_WHITE, 0.85f, srv_str);
        y += 30;

        if (audio) {
            draw_text(lx, y + 14, COL_GRAY, 0.85f, "Ring buf:");
            float fill = audio_fill_ratio(audio);
            int bar_w = 200;
            draw_slider(lx + 170, y + 8, bar_w, 10, fill, COL_SLIDER_FG, COL_SLIDER_BG);
            char rbuf[64];
            snprintf(rbuf, sizeof(rbuf), " %.0f%%  U:%d  O:%d",
                     fill * 100.f, audio->stat_underruns, audio->stat_overflows);
            draw_text(lx + 170 + bar_w, y + 14, COL_DIM, 0.75f, rbuf);
            y += 30;

            draw_text(lx, y + 14, COL_GRAY, 0.85f, "Prerolled:");
            draw_text(lx + 170, y + 14,
                      audio->prerolled ? COL_GREEN : COL_ORANGE, 0.85f,
                      audio->prerolled ? "yes" : "buffering...");
            y += 30;
        }
    }

    y += 20;
    draw_text(lx, y + 14, COL_DIM, 0.75f, "Snapcast Vita v1.0 - PS Vita Snapcast Client");
}

/* --- Main draw function --- */

void gui_draw(AppState *state, AudioContext *audio) {
    vita2d_start_drawing();
    vita2d_clear_screen();

    draw_top_bar(state);

    switch (state->current_screen) {
        case SCREEN_CONNECT:  draw_connect_screen(state); break;
        case SCREEN_PLAYER:   draw_player_screen(state, audio); break;
        case SCREEN_DEVICES:  draw_devices_screen(state); break;
        case SCREEN_GROUPS:   draw_groups_screen(state); break;
        case SCREEN_SETTINGS: draw_settings_screen(state, audio); break;
        default: break;
    }

    draw_bottom_bar(state);

    vita2d_end_drawing();
    if (ime_open)
        vita2d_common_dialog_update();
    vita2d_swap_buffers();
    vita2d_wait_rendering_done();
}

/* --- IME helpers --- */

static void open_ime(const char *title, const char *initial, int field_id) {
    a2w(title, ime_title_buf, 64);
    a2w(initial, ime_initial_buf, 256);
    memset(ime_result_buf, 0, sizeof(ime_result_buf));
    ime_field_id = field_id;

    SceImeDialogParam param;
    sceImeDialogParamInit(&param);
    param.supportedLanguages = 0x0001FFFF;
    param.languagesForced = SCE_TRUE;
    param.type = SCE_IME_TYPE_BASIC_LATIN;
    param.option = 0;
    param.title = ime_title_buf;
    param.maxTextLength = 255;
    param.initialText = ime_initial_buf;
    param.inputTextBuffer = ime_result_buf;

    sceImeDialogInit(&param);
    ime_open = 1;
}

int gui_ime_active(void) {
    return ime_open;
}

int gui_ime_update(AppState *state, NetContext *net) {
    if (!ime_open) return 0;

    SceCommonDialogStatus status = sceImeDialogGetStatus();
    if (status == SCE_COMMON_DIALOG_STATUS_FINISHED) {
        SceImeDialogResult result;
        memset(&result, 0, sizeof(result));
        sceImeDialogGetResult(&result);

        if (result.button == SCE_IME_DIALOG_BUTTON_ENTER) {
            char text[256];
            w2a(ime_result_buf, text, 256);

            switch (state->current_screen) {
                case SCREEN_CONNECT:
                    if (ime_field_id == 0) {
                        memset(state->config.server_ip, 0, MAX_IP_LEN);
                        memcpy(state->config.server_ip, text,
                               strlen(text) < MAX_IP_LEN - 1 ? strlen(text) : MAX_IP_LEN - 1);
                    } else if (ime_field_id == 1) {
                        int p = atoi(text);
                        if (p > 0 && p < 65536) state->config.stream_port = p;
                    }
                    break;
                case SCREEN_SETTINGS:
                    if (ime_field_id == 0) {
                        memset(state->config.client_name, 0, MAX_NAME_LEN);
                        memcpy(state->config.client_name, text,
                               strlen(text) < MAX_NAME_LEN - 1 ? strlen(text) : MAX_NAME_LEN - 1);
                        if (state->conn_state == CONN_CONNECTED && state->our_client_id[0])
                            net_rpc_set_name(net, state->our_client_id, text);
                    }
                    break;
                default:
                    break;
            }
        }

        sceImeDialogTerm();
        ime_open = 0;
        return 1;
    }
    return 0;
}

/* --- Count total clients for device list navigation --- */

static int count_total_clients(AppState *state) {
    int total = 0;
    for (int g = 0; g < state->server.group_count; g++)
        total += state->server.groups[g].client_count;
    return total;
}

/* Find the Nth client across all groups */
static SnapClient *find_client_by_index(AppState *state, int idx) {
    int i = 0;
    for (int g = 0; g < state->server.group_count; g++) {
        for (int c = 0; c < state->server.groups[g].client_count; c++) {
            if (i == idx) return &state->server.groups[g].clients[c];
            i++;
        }
    }
    return NULL;
}

/* --- Accelerating button-repeat helper ---
 * Call with a per-direction counter that the caller increments each frame the
 * button is held and resets to 0 on release.  Returns non-zero when an action
 * should fire this frame:
 *   frame 0          → fires immediately (first press)
 *   frames 1-19      → silent initial delay (~0.3 s @ 60 fps)
 *   frames 20-139    → slow repeat every 10 frames  (~6 /s)
 *   frames 140-259   → medium repeat every 4 frames (~15 /s)
 *   frames 260+      → fast repeat every 2 frames   (~30 /s)
 */
static int btn_held_should_fire(int held_frames) {
    if (held_frames == 0) return 1;
    if (held_frames < 20) return 0;
    int t = held_frames - 20;
    if (t < 120) return (t % 10) == 0;
    if (t < 240) return (t % 4)  == 0;
    return (t % 2) == 0;
}

/* --- Input handling --- */

void gui_handle_input(AppState *state, NetContext *net, AudioContext *audio) {
    /* Per-direction hold counters for accelerating repeat */
    static int player_held_l = 0, player_held_r = 0;
    static int devices_held_l = 0, devices_held_r = 0;
    static int settings_held_l = 0, settings_held_r = 0;

    pad_old = pad;
    sceCtrlPeekBufferPositive(0, &pad, 1);
    sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);

    if (ime_open) return;

    /* L/R to switch tabs */
    if (BTN_PRESSED(SCE_CTRL_LTRIGGER)) {
        state->current_screen = (state->current_screen + SCREEN_COUNT - 1) % SCREEN_COUNT;
        state->selected_item = 0;
    }
    if (BTN_PRESSED(SCE_CTRL_RTRIGGER)) {
        state->current_screen = (state->current_screen + 1) % SCREEN_COUNT;
        state->selected_item = 0;
    }

    switch (state->current_screen) {

    case SCREEN_CONNECT: {
        if (BTN_PRESSED(SCE_CTRL_UP) && state->selected_item > 0) state->selected_item--;
        if (BTN_PRESSED(SCE_CTRL_DOWN) && state->selected_item < 2) state->selected_item++;

        if (BTN_PRESSED(SCE_CTRL_CROSS)) {
            if (state->selected_item == 0)
                open_ime("Server IP", state->config.server_ip, 0);
            else if (state->selected_item == 1) {
                char port_str[16];
                snprintf(port_str, sizeof(port_str), "%d", state->config.stream_port);
                open_ime("Port", port_str, 1);
            }
        }

        if (BTN_PRESSED(SCE_CTRL_START) || (state->selected_item == 2 && BTN_PRESSED(SCE_CTRL_CROSS))) {
            if (state->conn_state == CONN_CONNECTED || state->conn_state == CONN_RECONNECTING) {
                state->conn_state = CONN_DISCONNECTED;
                state->auto_reconnect = 0;
                state->reconnect_attempts = 0;
                audio_stop(audio);
                net_stream_disconnect(net);
                net_control_disconnect(net);
            } else if (state->conn_state != CONN_CONNECTING) {
                state->conn_state = CONN_CONNECTING;
            }
        }
        break;
    }

    case SCREEN_PLAYER: {
        SnapStream *stream = find_our_stream(state);

        if (BTN_PRESSED(SCE_CTRL_CROSS) && stream && stream->properties.can_control) {
            net_rpc_stream_control(net, stream->id, "playPause");
        }
        if (BTN_PRESSED(SCE_CTRL_SQUARE) && stream && stream->properties.can_control) {
            net_rpc_stream_control(net, stream->id, "stop");
        }
        if (BTN_PRESSED(SCE_CTRL_TRIANGLE)) {
            state->volume_muted = !state->volume_muted;
            if (state->our_client_id[0])
                net_rpc_set_volume(net, state->our_client_id,
                                   state->volume_percent, state->volume_muted);
            audio_set_volume(audio, state->volume_percent, state->volume_muted);
        }

        if (pad.buttons & SCE_CTRL_LEFT) {
            if (btn_held_should_fire(player_held_l) && state->volume_percent > 0) {
                state->volume_percent--;
                if (state->our_client_id[0])
                    net_rpc_set_volume(net, state->our_client_id,
                                       state->volume_percent, state->volume_muted);
                audio_set_volume(audio, state->volume_percent, state->volume_muted);
            }
            player_held_l++;
        } else {
            player_held_l = 0;
        }
        if (pad.buttons & SCE_CTRL_RIGHT) {
            if (btn_held_should_fire(player_held_r) && state->volume_percent < 100) {
                state->volume_percent++;
                if (state->our_client_id[0])
                    net_rpc_set_volume(net, state->our_client_id,
                                       state->volume_percent, state->volume_muted);
                audio_set_volume(audio, state->volume_percent, state->volume_muted);
            }
            player_held_r++;
        } else {
            player_held_r = 0;
        }

        if (BTN_PRESSED(SCE_CTRL_UP) && stream && stream->properties.can_go_previous)
            net_rpc_stream_control(net, stream->id, "previous");
        if (BTN_PRESSED(SCE_CTRL_DOWN) && stream && stream->properties.can_go_next)
            net_rpc_stream_control(net, stream->id, "next");
        break;
    }

    case SCREEN_DEVICES: {
        int total = count_total_clients(state);
        if (BTN_PRESSED(SCE_CTRL_UP) && state->selected_item > 0) state->selected_item--;
        if (BTN_PRESSED(SCE_CTRL_DOWN) && state->selected_item < total - 1) state->selected_item++;

        SnapClient *cli = find_client_by_index(state, state->selected_item);
        if (cli) {
            if (BTN_PRESSED(SCE_CTRL_TRIANGLE)) {
                cli->volume.muted = !cli->volume.muted;
                net_rpc_set_volume(net, cli->id, cli->volume.percent, cli->volume.muted);
                if (strcmp(cli->id, state->our_client_id) == 0) {
                    state->volume_muted = cli->volume.muted;
                    audio_set_volume(audio, state->volume_percent, state->volume_muted);
                }
            }
            if (pad.buttons & SCE_CTRL_LEFT) {
                if (btn_held_should_fire(devices_held_l) && cli->volume.percent > 0) {
                    cli->volume.percent--;
                    net_rpc_set_volume(net, cli->id, cli->volume.percent, cli->volume.muted);
                    if (strcmp(cli->id, state->our_client_id) == 0) {
                        state->volume_percent = cli->volume.percent;
                        audio_set_volume(audio, state->volume_percent, state->volume_muted);
                    }
                }
                devices_held_l++;
            } else {
                devices_held_l = 0;
            }
            if (pad.buttons & SCE_CTRL_RIGHT) {
                if (btn_held_should_fire(devices_held_r) && cli->volume.percent < 100) {
                    cli->volume.percent++;
                    net_rpc_set_volume(net, cli->id, cli->volume.percent, cli->volume.muted);
                    if (strcmp(cli->id, state->our_client_id) == 0) {
                        state->volume_percent = cli->volume.percent;
                        audio_set_volume(audio, state->volume_percent, state->volume_muted);
                    }
                }
                devices_held_r++;
            } else {
                devices_held_r = 0;
            }
        }
        break;
    }

    case SCREEN_GROUPS: {
        if (BTN_PRESSED(SCE_CTRL_UP) && state->selected_group > 0) state->selected_group--;
        if (BTN_PRESSED(SCE_CTRL_DOWN) && state->selected_group < state->server.group_count - 1)
            state->selected_group++;

        if (BTN_PRESSED(SCE_CTRL_TRIANGLE)) {
            SnapGroup *grp = &state->server.groups[state->selected_group];
            grp->muted = !grp->muted;
            net_rpc_set_group_mute(net, grp->id, grp->muted);
        }

        /* Cycle through streams for the selected group */
        if (BTN_PRESSED(SCE_CTRL_SQUARE) && state->server.stream_count > 0) {
            SnapGroup *grp = &state->server.groups[state->selected_group];
            int cur_idx = -1;
            for (int s = 0; s < state->server.stream_count; s++) {
                if (strcmp(state->server.streams[s].id, grp->stream_id) == 0) {
                    cur_idx = s;
                    break;
                }
            }
            int next_idx = (cur_idx + 1) % state->server.stream_count;
            net_rpc_set_group_stream(net, grp->id, state->server.streams[next_idx].id);
        }
        break;
    }

    case SCREEN_SETTINGS: {
        if (BTN_PRESSED(SCE_CTRL_UP) && state->selected_item > 0) state->selected_item--;
        if (BTN_PRESSED(SCE_CTRL_DOWN) && state->selected_item < 1) state->selected_item++;

        if (BTN_PRESSED(SCE_CTRL_CROSS)) {
            if (state->selected_item == 0)
                open_ime("Client Name", state->config.client_name, 0);
        }

        if (state->selected_item == 1) {
            if (pad.buttons & SCE_CTRL_LEFT) {
                if (btn_held_should_fire(settings_held_l)) {
                    state->config.latency_ms--;
                    if (state->conn_state == CONN_CONNECTED && state->our_client_id[0])
                        net_rpc_set_latency(net, state->our_client_id, state->config.latency_ms);
                }
                settings_held_l++;
            } else {
                settings_held_l = 0;
            }
            if (pad.buttons & SCE_CTRL_RIGHT) {
                if (btn_held_should_fire(settings_held_r)) {
                    state->config.latency_ms++;
                    if (state->conn_state == CONN_CONNECTED && state->our_client_id[0])
                        net_rpc_set_latency(net, state->our_client_id, state->config.latency_ms);
                }
                settings_held_r++;
            } else {
                settings_held_r = 0;
            }
        }
        break;
    }

    default: break;
    }
}
