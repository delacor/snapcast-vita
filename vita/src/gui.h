#ifndef SNAPVITA_GUI_H
#define SNAPVITA_GUI_H

#include "types.h"
#include "network.h"
#include "audio.h"

#define SCREEN_W 960
#define SCREEN_H 544
#define TOP_BAR_H 44
#define BOTTOM_BAR_H 36
#define CONTENT_Y TOP_BAR_H
#define CONTENT_H (SCREEN_H - TOP_BAR_H - BOTTOM_BAR_H)

int  gui_init(void);
void gui_fini(void);
void gui_draw(AppState *state, AudioContext *audio);
void gui_handle_input(AppState *state, NetContext *net, AudioContext *audio);
int  gui_ime_active(void);
int  gui_ime_update(AppState *state, NetContext *net);

#endif /* SNAPVITA_GUI_H */
