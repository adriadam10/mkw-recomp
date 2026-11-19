// A mod's settings, per player (mod_settings.c): the store behind MkwPluginApi.setting_get / setting_set and the
// options page's widgets (MkwPage). <player folder>/mod-settings/<mod>.ini: outside the mod's folder, so updating or
// importing the mod again keeps them; inside the player's, so each player has their own. Any thread.
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { MOD_SETTING_KEY_MAX = 64, MOD_SETTING_VALUE_MAX = 1024 };

int mod_settings_key_ok(const char *key);   // letters, digits, '.', '_', '-'; 1 to 64 of them
// key's value into out (NUL-terminated, cut at cap); 1 when set, 0 when unset or an argument is not valid.
int mod_settings_get(const char *player_dir, const char *mod, const char *key, char *out, size_t cap);
// value (one line, at most 1024 bytes; NULL removes the key) stored and the file written; 1 done.
int mod_settings_set(const char *player_dir, const char *mod, const char *key, const char *value);

#ifdef __cplusplus
}
#endif
