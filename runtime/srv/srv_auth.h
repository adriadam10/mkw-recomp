// Sign-in (Auth/IssuePrearrangedUserToken) and the bearer tokens it hands out: HMAC-SHA256-signed with a key made on
// this PC (<data>/token.key), so a token cannot be forged and stays valid when the server starts again.
#pragma once
#include <stddef.h>
#include <stdint.h>

int srv_auth_start(void);   // 0 when the token key could not be made or read
// The caller of a call from its "authorization: bearer <token>" header; "" when absent, forged or malformed.
void srv_auth_uid(const char *authorization, char *uid, size_t cap);
uint64_t srv_uid_code(const char *uid);   // the friend code a uid was made from, 0 for an anonymous one
// A signed token for uid that no call takes as its bearer (use is not "a"), unique (a random id in it): a member's
// credential, a Gamesync token, a relay password. The game keeps them as opaque strings; the JWT shape of the
// sign-in's tokens is kept in case it looks. 0 when it could not be made.
enum { TOKEN_MAX = 400 };
int srv_auth_token(char *out, size_t cap, const char *uid, const char *use);
// SHA-256 of a player's secret, lowercase hex (out: 65 bytes).
void srv_secret_hash(const char *secret, char out[65]);
int srv_secret_equal(const char *a, const char *b);   // constant time
