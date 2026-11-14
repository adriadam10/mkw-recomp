// The online server's accounts and friendships, kept in <data> (srv_data_dir) as small text files written whole and
// atomically: accounts.tsv (one account per friend code), links.tsv (who added whom, who declined whom), settings.txt
// (key=value). Only a one-way hash of each player's secret is kept.
#pragma once
#include <stdint.h>

typedef struct {
    uint64_t code;      // the friend code, 12 digits, never 0
    char secret[65];    // SHA-256 of the player's secret, lowercase hex
    char name[64];      // the in-game name last seen (UTF-8), "" before any
    char tenant[64];    // the game the player last signed in to
    int64_t created, seen;
    int approved, banned;
} SrvAccount;

void srv_store_load(void);
void srv_store_save_soon(void);          // after a change: written within a second (srv_store_tick)
void srv_store_tick(uint64_t now_ms);

SrvAccount *srv_account(uint64_t code);  // NULL when unknown
SrvAccount *srv_account_add(uint64_t code, const char *secret_hash, int approved);   // written at once

// a added b (a friend request from a until b adds a back), or a declined b. Times in seconds since 1970.
int srv_added(uint64_t a, uint64_t b);
double srv_added_since(uint64_t a, uint64_t b);
int srv_adds_of(uint64_t a);
void srv_add(uint64_t a, uint64_t b);
void srv_unadd(uint64_t a, uint64_t b);
int srv_declined(uint64_t a, uint64_t b);
void srv_decline(uint64_t a, uint64_t b);
void srv_undecline(uint64_t a, uint64_t b);

// a's friends (both added each other), oldest friendship first; requests a received and sent, newest first. Return
// how many went into out (at most max).
int srv_friends(uint64_t a, uint64_t *out, int max);
int srv_received(uint64_t a, uint64_t *out, int max);
int srv_sent(uint64_t a, uint64_t *out, int max);

// settings.txt: open, approval or closed (registration); the sign-in limit per address and minute; friends per player.
const char *srv_setting(const char *key, const char *fallback);
int srv_setting_int(const char *key, int fallback, int lo, int hi);
