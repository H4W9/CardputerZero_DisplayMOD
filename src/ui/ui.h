#pragma once
#include "lvgl.h"
void ui_create(lv_group_t *group);
void ui_create_error(lv_group_t *group, const char *msg);
/* Implemented by main: leave the app. */
void ui_request_exit(void);
