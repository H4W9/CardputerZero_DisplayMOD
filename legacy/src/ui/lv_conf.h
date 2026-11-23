/* LVGL v9.2 configuration for Display MOD (CardputerZero, 320x170 RGB565). */
#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

#define LV_COLOR_DEPTH            16
#define LV_USE_STDLIB_MALLOC      LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_STRING      LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_SPRINTF     LV_STDLIB_BUILTIN
#define LV_MEM_SIZE               (256U * 1024U)   /* two views: built-in + EXT */
#define LV_DEF_REFR_PERIOD        20
#define LV_DPI_DEF                130

#define LV_USE_OS                 LV_OS_NONE
#define LV_USE_DRAW_SW            1
#define LV_USE_DRAW_SW_ASM        LV_DRAW_SW_ASM_NONE
#define LV_DRAW_SW_COMPLEX        1

#define LV_USE_LOG                0
#define LV_USE_ASSERT_NULL        1
#define LV_USE_ASSERT_MALLOC      1

#ifndef LV_USE_LINUX_FBDEV
#define LV_USE_LINUX_FBDEV        1
#endif
#define LV_LINUX_FBDEV_BSD        0
#define LV_LINUX_FBDEV_RENDER_MODE LV_DISPLAY_RENDER_MODE_PARTIAL
#define LV_LINUX_FBDEV_BUFFER_COUNT 0
#define LV_LINUX_FBDEV_BUFFER_SIZE 60
#define LV_USE_EVDEV              0

#define LV_FONT_MONTSERRAT_10     1
#define LV_FONT_MONTSERRAT_12     1
#define LV_FONT_MONTSERRAT_14     1
#define LV_FONT_MONTSERRAT_16     1
#define LV_FONT_DEFAULT           &lv_font_montserrat_12

#define LV_USE_THEME_DEFAULT      1
#define LV_THEME_DEFAULT_DARK     1

#define LV_BUILD_EXAMPLES         0
#define LV_USE_DEMO_WIDGETS       0

#endif /* LV_CONF_H */
