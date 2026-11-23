#pragma once
#include "lvgl.h"
/* disps[0] is the built-in screen (keyboard focus); disps[1], if given, the EXT display. */
void ui_create(lv_group_t *group, lv_display_t *const *disps, int n);
void ui_create_error(lv_group_t *group, lv_display_t *const *disps, int n, const char *msg);
/* Implemented by main: leave the app. */
void ui_request_exit(void);
