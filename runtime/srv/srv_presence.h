// Players online and their presence (srv_presence.c): who is online (calls and open streams), the attributes each
// player's game publishes (PresenceService/KeepAlive), and the friend list and friends' presences streamed to each
// player (Friends SubscribeFriendUsers, SubscribePresences).
#pragma once
#include <stdint.h>

// build: the game's build id from the identity string, "" when unknown (matchmaking and rooms keep builds apart).
void srv_presence_signin(const char *uid, uint64_t code, const char *tenant, const char *build);
int srv_online(uint64_t code);           // in any game: for the friends API
int srv_uid_online(const char *uid);     // a ticket whose player is gone is cancelled (codeless players too)
// What the services know of a player, "" when unknown: the game signed in to, its build, the game version it last
// showed in a ticket or a room (its batching_key, else rom_id).
const char *srv_player_tenant(const char *uid);
const char *srv_player_build(const char *uid);
const char *srv_player_version(const char *uid);
void srv_player_set_version(const char *uid, const char *version);
