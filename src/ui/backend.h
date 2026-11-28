/* DisplayMOD settings backend: talks to displaymod-ctl (device) or a fake (tests). */
#pragma once

#define DM_MAX_STATUS 12

typedef struct {
    char key[24];
    char val[48];
} dm_pair_t;

/* Load all settings. Returns 0 on success, -1 if the helper is unreachable. */
int dm_load(void);
/* Current value of a setting ("" if unknown). */
const char *dm_get(const char *key);
/* Start applying KEY=VAL in the background. Returns 0 if started. */
int dm_set_async(const char *key, const char *val);
/* Change several settings and apply them once, in the background. */
int dm_set_many_async(const char *const *kv, int pairs);
/* Re-apply settings in the background (re-checks for the screen). */
int dm_apply_async(void);
/* Start uninstall in the background. */
int dm_uninstall_async(void);
/* Poll a background job: 0 = still running, 1 = finished (rc in *rc). */
int dm_poll(int *rc);
/* Status lines (label|value). Returns count. */
int dm_status(dm_pair_t *out, int max);
/* BigTermCLI history (the user's own file, no root needed). */
int dm_history_count(void);
void dm_history_clear(void);
/* Read the saved commands; returns how many. Item 0 is the newest. The
 * strings stay valid until the next load or delete. */
int dm_history_load(void);
const char *dm_history_item(int i);
/* Delete saved command i (as numbered by the last load). 0 on success. */
int dm_history_delete(int i);
/* 1 if the EXT display driver is loaded. */
int dm_panel_up(void);
/* EXT display state: 1 = detected and running, 0 = off, -1 = not detected. */
int dm_panel_state(void);
