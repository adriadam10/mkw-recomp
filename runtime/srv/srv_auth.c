// Sign-in and bearer tokens (srv_auth.h), as the protocol description's "Identity, sign-in and accounts" says: the
// identity string our runtime puts in the game's sign-in request (droplet-online-v1;code=...;secret=...;friends=...),
// accounts bound to the first secret that uses a code, registration open / approval / closed, a sign-in limit per
// address. Tokens: a JWT-shaped string (header.payload.signature) signed with HMAC-SHA256.
#include "srv_auth.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef _WIN32
#include <sys/stat.h>
#endif

#include <psa/crypto.h>

#include "platform/platform.h"
#include "srv.h"
#include "srv_pb.h"
#include "srv_presence.h"
#include "srv_store.h"
#include "srv_svc.h"

enum { LIST_MAX = 4096 };   // a sign-in's friend list: 200 codes and their commas fit

static mbedtls_svc_key_id_t token_key;
static int key_ready;
static uint64_t anon_next = 1;

// ---- base64url (RFC 4648 section 5, no padding)
static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static size_t b64url(char *out, size_t cap, const uint8_t *in, size_t n) {
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        int chars = n - i >= 3 ? 4 : (int)(n - i) + 1;
        for (int k = 0; k < chars && o + 1 < cap; k++) out[o++] = B64[(v >> (18 - 6 * k)) & 63];
    }
    out[o] = 0;
    return o;
}

static int b64val(char c) {
    const char *p = c ? strchr(B64, c) : NULL;
    return p ? (int)(p - B64) : -1;
}

// Decoded length, or -1 on a character outside the alphabet or no room.
static int unb64url(uint8_t *out, size_t cap, const char *in, size_t n) {
    size_t o = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < n; i++) {
        int v = b64val(in[i]);
        if (v < 0) return -1;
        acc = acc << 6 | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= cap) return -1;
            out[o++] = (uint8_t)(acc >> bits);
        }
    }
    return (int)o;
}

// ---- secrets and tokens
void srv_secret_hash(const char *secret, char out[65]) {
    uint8_t h[32];
    size_t n = 0;
    out[0] = 0;
    if (psa_hash_compute(PSA_ALG_SHA_256, (const uint8_t *)secret, strlen(secret), h, sizeof h, &n) != PSA_SUCCESS)
        return;
    for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", h[i]);
}

int srv_secret_equal(const char *a, const char *b) {
    size_t n = strlen(a);
    if (n != strlen(b)) return 0;
    unsigned d = 0;
    for (size_t i = 0; i < n; i++) d |= (unsigned)(a[i] ^ b[i]);
    return d == 0;
}

int srv_auth_start(void) {
    char path[1100];
    snprintf(path, sizeof path, "%s/token.key", srv_data_dir());
    uint64_t n = 0;
    uint8_t *k = plat_file_read(path, &n), fresh[32];
    if (!k || n != 32) {   // first start: the key the server signs its tokens with, this PC's alone
        free(k);
        k = NULL;
        if (!plat_random(fresh, sizeof fresh) || !plat_file_write_atomic(path, fresh, sizeof fresh)) return 0;
#ifndef _WIN32
        chmod(path, 0600);
#endif
    }
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&a, PSA_KEY_TYPE_HMAC);
    psa_set_key_usage_flags(&a, PSA_KEY_USAGE_SIGN_MESSAGE | PSA_KEY_USAGE_VERIFY_MESSAGE);
    psa_set_key_algorithm(&a, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_status_t s = psa_import_key(&a, k ? k : fresh, 32, &token_key);
    if (k) {
        memset(k, 0, 32);
        free(k);
    }
    memset(fresh, 0, sizeof fresh);
    key_ready = s == PSA_SUCCESS;
    return key_ready;
}

static int sign(const char *data, size_t n, uint8_t mac[32]) {
    size_t got = 0;
    return key_ready &&
           psa_mac_compute(token_key, PSA_ALG_HMAC(PSA_ALG_SHA_256), (const uint8_t *)data, n, mac, 32, &got) ==
               PSA_SUCCESS &&
           got == 32;
}

// kind "a": an access token (the bearer of calls); "r": the refresh token, never taken as a bearer; others: see
// srv_auth_token. jti: a random id that makes the token unique, NULL for none.
static int make_token(char *out, size_t cap, const char *uid, const char *kind, const char *jti) {
    char json[224], part[320];
    snprintf(json, sizeof json, "{\"alg\":\"HS256\",\"typ\":\"JWT\"}");
    size_t n = b64url(out, cap, (const uint8_t *)json, strlen(json));
    snprintf(json, sizeof json, "{\"sub\":\"%s\",\"use\":\"%s\",\"iat\":%lld%s%s%s}", uid, kind, (long long)time(NULL),
             jti ? ",\"jti\":\"" : "", jti ? jti : "", jti ? "\"" : "");
    b64url(part, sizeof part, (const uint8_t *)json, strlen(json));
    n += (size_t)snprintf(out + n, cap - n, ".%s", part);
    uint8_t mac[32];
    if (n >= cap || !sign(out, n, mac)) return 0;
    b64url(part, sizeof part, mac, sizeof mac);
    return (size_t)snprintf(out + n, cap - n, ".%s", part) < cap - n;
}

int srv_auth_token(char *out, size_t cap, const char *uid, const char *use) {
    uint8_t r[16];
    char jti[33];
    if (!plat_random(r, sizeof r)) return 0;
    for (int i = 0; i < 16; i++) snprintf(jti + 2 * i, 3, "%02x", r[i]);
    return make_token(out, cap, uid, use, jti);
}

static int starts_nocase(const char *s, const char *prefix) {
    for (; *prefix; s++, prefix++)
        if (tolower((unsigned char)*s) != *prefix) return 0;
    return 1;
}

void srv_auth_uid(const char *auth, char *uid, size_t cap) {
    uid[0] = 0;
    while (*auth == ' ') auth++;
    if (!starts_nocase(auth, "bearer ")) return;
    const char *t = auth + 7;
    while (*t == ' ') t++;
    const char *d1 = strchr(t, '.'), *d2 = d1 ? strchr(d1 + 1, '.') : NULL;
    if (!d2) return;
    size_t siglen = strcspn(d2 + 1, " \r\n");
    uint8_t mac[32], got[40], payload[200];
    if (!sign(t, (size_t)(d2 - t), mac) || unb64url(got, sizeof got, d2 + 1, siglen) != 32) return;
    unsigned diff = 0;
    for (int i = 0; i < 32; i++) diff |= mac[i] ^ got[i];
    if (diff) return;   // not signed by this server
    int n = unb64url(payload, sizeof payload - 1, d1 + 1, (size_t)(d2 - d1 - 1));
    if (n <= 0) return;
    payload[n] = 0;
    const char *sub = strstr((const char *)payload, "\"sub\":\"u-");
    if (!sub || !strstr((const char *)payload, "\"use\":\"a\"") || strlen(sub + 7) < 19 || sub[25] != '"') return;
    snprintf(uid, cap, "%.18s", sub + 7);
}

uint64_t srv_uid_code(const char *uid) {
    if (strncmp(uid, "u-", 2) || strlen(uid) != 18) return 0;
    char *end;
    uint64_t v = strtoull(uid + 2, &end, 16);
    return *end || v > 999999999999ull ? 0 : v;
}

// ---- the identity string
typedef struct {
    uint64_t code;
    char secret[129], build[65];
    char friends[LIST_MAX], removed[LIST_MAX];   // comma-separated codes
} Identity;

static int twelve_digits(const char *s, size_t n) {
    if (n != 12) return 0;
    for (size_t i = 0; i < n; i++)
        if (!isdigit((unsigned char)s[i])) return 0;
    return 1;
}

// The game's build id, kept when it is 8 to 64 hex digits (lowercased): the exact build matchmaking and rooms match.
static void keep_build(char out[65], const char *v) {
    size_t n = strlen(v);
    if (n < 8 || n > 64) return;
    for (size_t i = 0; i < n; i++)
        if (!isxdigit((unsigned char)v[i])) return;
    for (size_t i = 0; i <= n; i++) out[i] = (char)tolower((unsigned char)v[i]);
}

static int parse_identity(const char *s, size_t len, Identity *id) {
    memset(id, 0, sizeof *id);
    char buf[2 * LIST_MAX + 512];
    if (len >= sizeof buf) return 0;
    memcpy(buf, s, len);
    buf[len] = 0;
    char *item = buf, *next;
    int first = 1;
    for (; item; item = next) {
        next = strchr(item, ';');
        if (next) *next++ = 0;
        if (first) {
            if (strcmp(item, "droplet-online-v1") && strcmp(item, "ounce2dn-online-v1")) return 0;
            first = 0;
            continue;
        }
        char *eq = strchr(item, '=');
        if (!eq) continue;
        *eq = 0;
        const char *v = eq + 1;
        if (!strcmp(item, "code") && twelve_digits(v, strlen(v))) id->code = strtoull(v, NULL, 10);
        else if (!strcmp(item, "secret")) snprintf(id->secret, sizeof id->secret, "%s", v);
        else if (!strcmp(item, "build")) keep_build(id->build, v);
        else if (!strcmp(item, "friends")) snprintf(id->friends, sizeof id->friends, "%s", v);
        else if (!strcmp(item, "removed")) snprintf(id->removed, sizeof id->removed, "%s", v);
    }
    return id->code && strlen(id->secret) >= 16;
}

static int listed_in(const char *list, uint64_t code) {
    char want[16];
    snprintf(want, sizeof want, "%012llu", (unsigned long long)code);
    for (const char *p = list; *p;) {
        size_t n = strcspn(p, ",");
        if (n == 12 && !memcmp(p, want, 12)) return 1;
        p += n + (p[n] == ',');
    }
    return 0;
}

// The adds a sign-in carries: each code in friends is "this player added that one" unless that player declined this
// one; codes only in removed take this player's add back. Nothing else is deleted (a reinstall's empty list is not a
// removal).
static void sign_in_friends(uint64_t me, const Identity *id) {
    int max = srv_setting_int("max_friends", 200, 1, 1000);
    for (const char *p = id->friends; *p;) {
        size_t n = strcspn(p, ",");
        if (twelve_digits(p, n)) {
            uint64_t f = strtoull(p, NULL, 10);
            if (f && f != me && !srv_declined(f, me) && !srv_added(me, f) && srv_adds_of(me) < max) srv_add(me, f);
        }
        p += n + (p[n] == ',');
    }
    for (const char *p = id->removed; *p;) {
        size_t n = strcspn(p, ",");
        if (twelve_digits(p, n)) {
            uint64_t f = strtoull(p, NULL, 10);
            if (!listed_in(id->friends, f)) srv_unadd(me, f);
        }
        p += n + (p[n] == ',');
    }
}

// ---- sign-in limit: attempts per address in the last minute
static struct {
    char ip[48];
    uint64_t at[64];
    int n;
} tries[64];

static int too_many(const char *ip, uint64_t now) {
    int limit = srv_setting_int("login_limit", 20, 1, 64), slot = -1, oldest = 0;
    for (int i = 0; i < 64; i++) {
        if (!strcmp(tries[i].ip, ip)) {
            slot = i;
            break;
        }
        if (!tries[i].n || (tries[oldest].n && tries[i].at[tries[i].n - 1] < tries[oldest].at[tries[oldest].n - 1]))
            oldest = i;
    }
    if (slot < 0) {   // a new address takes the place of the least recent one
        slot = oldest;
        memset(&tries[slot], 0, sizeof tries[slot]);
        snprintf(tries[slot].ip, sizeof tries[slot].ip, "%s", ip);
    }
    int k = 0;
    for (int i = 0; i < tries[slot].n; i++)
        if (now - tries[slot].at[i] < 60000) tries[slot].at[k++] = tries[slot].at[i];
    tries[slot].n = k;
    if (k >= limit) return 1;
    tries[slot].at[tries[slot].n++] = now;
    return 0;
}

// ---- Auth/IssuePrearrangedUserToken
static void put_now(PbOut *o, uint32_t num) {
    size_t m = pb_begin(o, num);
    pb_int64(o, 1, (int64_t)time(NULL));
    pb_end(o, m);
}

static void sign_in(SrvCall *c, const uint8_t *msg, size_t len) {
    PbField parent = {0}, cred = {0}, token = {0};
    char tenant[64] = "", parent_s[96];
    if (pb_find(msg, len, 1, &parent) == 1 && parent.wt == PB_LEN && parent.len > 8 && parent.len < 72 &&
        !memcmp(parent.data, "tenants/", 8)) {
        snprintf(tenant, sizeof tenant, "%.*s", (int)parent.len - 8, (const char *)parent.data + 8);
    } else {
        snprintf(tenant, sizeof tenant, "%s", srv_call_tenant(c));
    }
    if (!*tenant || strchr(tenant, '/')) {
        srv_call_end(c, ST_PERMISSION_DENIED, "no game named");
        return;
    }
    snprintf(parent_s, sizeof parent_s, "tenants/%s", tenant);
    if (atoi(srv_setting("maintenance", "0"))) {
        srv_call_end(c, ST_UNAVAILABLE, "maintenance");
        return;
    }
    if (too_many(srv_call_ip(c), srv_ms())) {
        srv_call_end(c, ST_RESOURCE_EXHAUSTED, "too many sign-ins");
        return;
    }
    static Identity id;   // 8 KiB: not on the stack
    int has = pb_find(msg, len, 3, &cred) == 1 && cred.wt == PB_LEN && pb_find(cred.data, cred.len, 1, &token) == 1 &&
              token.wt == PB_LEN && parse_identity((const char *)token.data, token.len, &id);
    const char *mode = srv_setting("registration", "open");
    char uid[24];
    uint64_t code = 0;
    if (has) {
        char hash[65];
        srv_secret_hash(id.secret, hash);
        SrvAccount *a = srv_account(id.code);
        if (a && !srv_secret_equal(a->secret, hash)) {
            srv_call_end(c, ST_PERMISSION_DENIED, "friend code belongs to another emulator");
            return;
        }
        if (!a) {
            if (!strcmp(mode, "closed")) {
                srv_call_end(c, ST_PERMISSION_DENIED, "not taking new players");
                return;
            }
            a = srv_account_add(id.code, hash, strcmp(mode, "approval") != 0);
            if (!a) {
                srv_call_end(c, ST_INTERNAL, "could not keep the account");
                return;
            }
            srv_log("new account %012llu%s", (unsigned long long)id.code, a->approved ? "" : " (waiting for approval)");
        }
        if (a->banned) {
            srv_call_end(c, ST_PERMISSION_DENIED, "banned");
            return;
        }
        if (!a->approved) {
            srv_call_end(c, ST_PERMISSION_DENIED, "waiting for approval");
            return;
        }
        code = a->code;
        a->seen = (int64_t)time(NULL);
        snprintf(a->tenant, sizeof a->tenant, "%s", tenant);
        srv_store_save_soon();
        sign_in_friends(code, &id);
        snprintf(uid, sizeof uid, "u-%016llx", (unsigned long long)code);
    } else {
        if (strcmp(mode, "open")) {
            srv_call_end(c, ST_PERMISSION_DENIED, "needs a friend code");
            return;
        }
        // anonymous: above every code's uid (codes stop at 999999999999 = 0xe8d4a50fff)
        snprintf(uid, sizeof uid, "u-%016llx", (unsigned long long)(0x1000000000000ull + anon_next++));
    }
    char user[160], access[400], refresh[400];
    snprintf(user, sizeof user, "%s/users/%s", parent_s, uid);
    if (!make_token(access, sizeof access, uid, "a", NULL) || !make_token(refresh, sizeof refresh, uid, "r", NULL)) {
        srv_call_end(c, ST_INTERNAL, "could not sign a token");
        return;
    }
    PbOut o = {0};
    size_t m = pb_begin(&o, 1);   // User
    pb_str(&o, 1, user);
    pb_str(&o, 2, "E");
    put_now(&o, 3);
    put_now(&o, 4);
    pb_end(&o, m);
    m = pb_begin(&o, 2);   // UserToken
    pb_str(&o, 1, user);
    pb_str(&o, 2, access);
    pb_str(&o, 3, refresh);
    size_t d = pb_begin(&o, 4);   // Duration: an hour
    pb_int64(&o, 1, 3600);
    pb_end(&o, d);
    pb_end(&o, m);
    if (o.err) {
        pb_free(&o);
        srv_call_end(c, ST_INTERNAL, "reply too large");
        return;
    }
    srv_presence_signin(uid, code, tenant, has ? id.build : "");
    srv_call_reply(c, o.buf, o.len);
    pb_free(&o);
    srv_log("%s signed in to %s as %s", srv_call_ip(c), tenant, code ? "a friend code" : "a guest");
    srv_call_end(c, ST_OK, NULL);
}

const SrvMethod SRV_SIGN_IN = {"Auth", "IssuePrearrangedUserToken", SRV_UNARY, 0, sign_in, NULL, NULL, NULL};
