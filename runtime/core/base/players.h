// The players of one install (the start screen's Profiles page): each its own console account, nickname and save, as
// the users of a console. The game still sees one account, the one played: the launcher picks it before the start.
//
//   <base>/                      the first player: the profile folder as it was before players existed (its save stays)
//   <base>/players/<n>/          every other player, n a number
//   <base>/players/deleted/      players taken off the list (their folder moved here, never deleted: a save is the
//                                player's progress, and a wrong click must not lose it)
//   <folder>/nickname.txt        the player's name, UTF-8; missing: PLAYER_DEFAULT_NAME
//
// base is the profile folder of main.c (MKW_PROFILE_DIR, profile/ in the selected installation, or --profile).
// mkw.ini's MKW_PLAYER names the player the next start plays: n, or empty for the first one.
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PLAYER_NAME_CHARS 10      // the console's limit for a user's nickname, in characters
#define PLAYER_NAME_BYTES 33      // nn::account::Nickname: up to 32 bytes of UTF-8 and the NUL
#define PLAYER_DEFAULT_NAME "Player"
#define PLAYERS_MAX 16

typedef struct {
    char id[12];                  // "" the first player, else the folder's number
    char name[PLAYER_NAME_BYTES];
} Player;

// The players, the first one first, then by number. Returns how many (at least 1).
int players_list(const char *base, Player *out, int cap);
// The folder of player id ("" the first one).
void player_dir(const char *base, const char *id, char *out, size_t cap);
// The name in folder dir: its nickname.txt, else PLAYER_DEFAULT_NAME. out holds PLAYER_NAME_BYTES.
void player_name(const char *dir, char *out);
// name cut to what the console takes: control characters dropped, spaces trimmed, at most PLAYER_NAME_CHARS
// characters and 32 bytes, whole UTF-8 sequences only. out holds PLAYER_NAME_BYTES; "" when nothing is left.
void player_name_clean(const char *name, char *out);
// A new player named name: its folder and nickname.txt; its id into id_out. 0 when the name is empty after
// player_name_clean, PLAYERS_MAX are there, or the folder cannot be made.
int player_create(const char *base, const char *name, char *id_out, size_t cap);
// 1 when the name (cleaned, not empty) is written.
int player_rename(const char *base, const char *id, const char *name);
// Moves player id's folder into players/deleted/; never the first one. 1 when moved.
int player_remove(const char *base, const char *id);
// The folder of the player mkw.ini's MKW_PLAYER names; base when it is empty or its folder is missing (a player
// removed, or a --profile / MKW_PROFILE_DIR of a test run without players).
void player_active_dir(const char *base, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
