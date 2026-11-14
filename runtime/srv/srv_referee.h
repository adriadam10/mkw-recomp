// MKW's race referee (the protocol description, "Race referee"), entirely in a game session's Gamesync documents: a
// player's request document gets a result document (status 0) in the same batch, so every game sees within its 36 s
// that the race is on; reports are gathered into an official order; the race closes (status 1, final order, Elo
// ratings) once every player still there is placed, or 120 s after the first report. Knockout Tour sections leave
// out the players knocked out by the earlier sections of the same tour.
// No platform code but srv_random (srv.h): tests/c/t_srv.c builds it alone.
#pragma once
#include <stdint.h>

#include "srv_docs.h"

enum { RACE_PLAYERS = 32, RACES_MAX = 32, RACE_GRACE_MS = 120000 };

typedef struct {
    int used, open, section;   // section: a Knockout Tour section's number, -1 for any other race
    uint64_t seq;              // acceptance order (the oldest closed race makes room for a new one)
    char uuid[40], request_id[128], tour[128];
    int64_t dropout_rank;      // Knockout: final positions (from 1) at or past it are knocked out, 0 none
    int n;                     // the race's player indices: idx[k] (ussid_map's keys) is player ussid[k]
    int64_t idx[RACE_PLAYERS], ussid[RACE_PLAYERS], rank[RACE_PLAYERS];
    uint8_t ranked[RACE_PLAYERS], retired[RACE_PLAYERS];
    int order[RACE_PLAYERS], norder;   // the official order: k of each index, in the order reports first listed it
    int final[RACE_PLAYERS], nfinal;   // at close
    uint64_t first_report_ms;          // 0: no report yet
} SrvRace;

typedef struct {
    SrvRace races[RACES_MAX];
    uint64_t seq;
} SrvReferee;

// After a batch on the session (the paths it changed are d->changed[from..]): requests accepted, reports recorded,
// races closed. The referee's own writes are a batch of their own, whose changes join d->changed.
void referee_batch(SrvReferee *r, SrvDocs *d, int from, uint64_t now_ms, int64_t now_s);
// Races whose first report is RACE_GRACE_MS old are closed whoever is missing. Run after every batch, and by the
// session's timer (when everybody waits, nobody writes).
void referee_tick(SrvReferee *r, SrvDocs *d, uint64_t now_ms, int64_t now_s);
// Elo with K = 24 over every pair: rating[i] and rank[i] (lower is better, equal ranks draw) of n players, their new
// ratings in out.
void referee_elo(const double *rating, const double *rank, int n, double *out);
