// The online server's accounts and friendships (srv_store.h). Small tables in memory, written whole to their files
// after a change: a new account or friendship at once (the server ends with the game, killed with it), the "last seen"
// kind of change within a second.
#include "srv_store.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "platform/platform.h"
#include "srv.h"

typedef struct {
    uint64_t a, b;
    double since;
    char kind;   // 'A': a added b; 'D': a declined b
} Link;

static SrvAccount **accts;   // pointers: an account's address stays the same as the table grows
static int nacct, cacct;
static Link *links;
static int nlink, clink;
static struct {
    char key[32], val[64];
} sets[32];
static int nset, dirty, loading;
static uint64_t dirty_since;

typedef struct {
    char *p;
    size_t n, cap;
} Text;

static void tput(Text *t, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (need < 0) return;
    if (t->n + (size_t)need + 1 > t->cap) {
        size_t cap = t->cap ? t->cap * 2 : 4096;
        while (cap < t->n + (size_t)need + 1) cap *= 2;
        char *p = realloc(t->p, cap);
        if (!p) return;
        t->p = p;
        t->cap = cap;
    }
    va_start(ap, fmt);
    vsnprintf(t->p + t->n, t->cap - t->n, fmt, ap);
    va_end(ap);
    t->n += (size_t)need;
}

// Names travel in a tab-separated line: tab, line ends and backslash escaped.
static void put_escaped(Text *t, const char *s) {
    for (; *s; s++) {
        if (*s == '\t') tput(t, "\\t");
        else if (*s == '\n') tput(t, "\\n");
        else if (*s == '\r') tput(t, "\\r");
        else if (*s == '\\') tput(t, "\\\\");
        else tput(t, "%c", *s);
    }
}

static void unescape(char *dst, size_t cap, const char *s) {
    size_t n = 0;
    for (; *s && n + 1 < cap; s++) {
        if (*s == '\\' && s[1]) {
            s++;
            dst[n++] = *s == 't' ? '\t' : *s == 'n' ? '\n' : *s == 'r' ? '\r' : *s;
        } else {
            dst[n++] = *s;
        }
    }
    dst[n] = 0;
}

static void file_path(char *out, size_t cap, const char *name) { snprintf(out, cap, "%s/%s", srv_data_dir(), name); }

// The line's tab-separated fields, cut in place; how many.
static int fields(char *line, char **f, int max) {
    int n = 0;
    while (n < max) {
        f[n++] = line;
        char *t = strchr(line, '\t');
        if (!t) break;
        *t = 0;
        line = t + 1;
    }
    return n;
}

static int hex64(const char *s) {
    if (strlen(s) != 64) return 0;
    for (; *s; s++)
        if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f'))) return 0;
    return 1;
}

static void each_line(const char *name, void (*fn)(char *line)) {
    char path[1100];
    file_path(path, sizeof path, name);
    uint64_t size = 0;
    char *text = (char *)plat_file_read(path, &size);   // NUL after the last byte
    if (!text) return;
    for (char *line = text, *end; *line; line = end) {
        end = strchr(line, '\n');
        if (end) *end++ = 0;
        else end = line + strlen(line);
        size_t n = strlen(line);
        if (n && line[n - 1] == '\r') line[n - 1] = 0;
        if (*line && *line != '#') fn(line);
    }
    free(text);
}

static void load_account(char *line) {
    char *f[8];
    if (fields(line, f, 8) != 8) return;
    uint64_t code = strtoull(f[0], NULL, 10);
    if (!code || code > 999999999999ull || !hex64(f[1]) || srv_account(code)) return;
    SrvAccount *a = srv_account_add(code, f[1], atoi(f[2]) != 0);
    if (!a) return;
    a->banned = atoi(f[3]) != 0;
    a->created = strtoll(f[4], NULL, 10);
    a->seen = strtoll(f[5], NULL, 10);
    unescape(a->tenant, sizeof a->tenant, f[6]);
    unescape(a->name, sizeof a->name, f[7]);
}

static void put_link(uint64_t a, uint64_t b, char kind, double since) {
    if (nlink == clink) {
        int cap = clink ? clink * 2 : 64;
        Link *l = realloc(links, (size_t)cap * sizeof *l);
        if (!l) return;
        links = l;
        clink = cap;
    }
    links[nlink++] = (Link){a, b, since, kind};
}

static void load_link(char *line) {
    char *f[4];
    if (fields(line, f, 4) != 4 || (f[0][0] != 'A' && f[0][0] != 'D')) return;
    uint64_t a = strtoull(f[1], NULL, 10), b = strtoull(f[2], NULL, 10);
    if (a && b && a != b) put_link(a, b, f[0][0], strtod(f[3], NULL));
}

static void load_setting(char *line) {
    char *eq = strchr(line, '=');
    if (!eq || nset == (int)(sizeof sets / sizeof *sets)) return;
    *eq = 0;
    snprintf(sets[nset].key, sizeof sets[nset].key, "%s", line);
    snprintf(sets[nset].val, sizeof sets[nset].val, "%s", eq + 1);
    nset++;
}

static void save(void) {
    if (loading) return;   // the files being read are not written over half-read
    Text t = {0};
    tput(&t, "# code\tsecret_sha256\tapproved\tbanned\tcreated\tseen\ttenant\tname\n");
    for (int i = 0; i < nacct; i++) {
        SrvAccount *a = accts[i];
        tput(&t, "%012llu\t%s\t%d\t%d\t%lld\t%lld\t", (unsigned long long)a->code, a->secret, a->approved, a->banned,
             (long long)a->created, (long long)a->seen);
        put_escaped(&t, a->tenant);
        tput(&t, "\t");
        put_escaped(&t, a->name);
        tput(&t, "\n");
    }
    char path[1100];
    file_path(path, sizeof path, "accounts.tsv");
    if (t.p && !plat_file_write_atomic(path, t.p, t.n)) srv_log("could not write %s", path);
    t.n = 0;
    tput(&t, "# A: a added b, D: a declined b\tsince\n");
    for (int i = 0; i < nlink; i++)
        tput(&t, "%c\t%012llu\t%012llu\t%.0f\n", links[i].kind, (unsigned long long)links[i].a,
             (unsigned long long)links[i].b, links[i].since);
    file_path(path, sizeof path, "links.tsv");
    if (t.p && !plat_file_write_atomic(path, t.p, t.n)) srv_log("could not write %s", path);
    free(t.p);
    dirty = 0;
}

void srv_store_load(void) {
    loading = 1;
    each_line("accounts.tsv", load_account);
    each_line("links.tsv", load_link);
    char path[1100];
    file_path(path, sizeof path, "settings.txt");
    if (!plat_file_exists(path)) {   // the defaults, written out so they can be seen and changed
        static const char DEFAULTS[] =
            "# The server's settings, read at its start.\n"
            "# registration: open (any new friend code plays at once), approval (new codes wait for the host), closed\n"
            "registration=open\n"
            "# sign-ins one address may make in a minute\n"
            "login_limit=20\n"
            "# friend requests one player may have\n"
            "max_friends=200\n";
        plat_file_write_atomic(path, DEFAULTS, sizeof DEFAULTS - 1);
    }
    each_line("settings.txt", load_setting);
    loading = 0;
    srv_log("accounts: %d, links: %d, registration %s", nacct, nlink, srv_setting("registration", "open"));
}

void srv_store_save_soon(void) {
    if (!dirty) dirty_since = srv_ms();
    dirty = 1;
}

void srv_store_tick(uint64_t now_ms) {
    if (dirty && now_ms - dirty_since >= 1000) save();
}

SrvAccount *srv_account(uint64_t code) {
    for (int i = 0; i < nacct; i++)
        if (accts[i]->code == code) return accts[i];
    return NULL;
}

SrvAccount *srv_account_add(uint64_t code, const char *secret_hash, int approved) {
    if (nacct == cacct) {
        int cap = cacct ? cacct * 2 : 32;
        SrvAccount **a = realloc(accts, (size_t)cap * sizeof *a);
        if (!a) return NULL;
        accts = a;
        cacct = cap;
    }
    SrvAccount *a = calloc(1, sizeof *a);
    if (!a) return NULL;
    a->code = code;
    snprintf(a->secret, sizeof a->secret, "%s", secret_hash);
    a->created = a->seen = (int64_t)time(NULL);
    a->approved = approved;
    accts[nacct++] = a;
    save();
    return a;
}

static Link *find(uint64_t a, uint64_t b, char kind) {
    for (int i = 0; i < nlink; i++)
        if (links[i].a == a && links[i].b == b && links[i].kind == kind) return &links[i];
    return NULL;
}

static void drop(uint64_t a, uint64_t b, char kind) {
    Link *l = find(a, b, kind);
    if (!l) return;
    *l = links[--nlink];
    save();
}

int srv_added(uint64_t a, uint64_t b) { return find(a, b, 'A') != NULL; }
double srv_added_since(uint64_t a, uint64_t b) {
    Link *l = find(a, b, 'A');
    return l ? l->since : 0;
}
int srv_declined(uint64_t a, uint64_t b) { return find(a, b, 'D') != NULL; }

int srv_adds_of(uint64_t a) {
    int n = 0;
    for (int i = 0; i < nlink; i++) n += links[i].a == a && links[i].kind == 'A';
    return n;
}

void srv_add(uint64_t a, uint64_t b) {
    if (a == b || find(a, b, 'A')) return;
    put_link(a, b, 'A', (double)time(NULL));
    save();
}
void srv_unadd(uint64_t a, uint64_t b) { drop(a, b, 'A'); }
void srv_decline(uint64_t a, uint64_t b) {
    if (a == b || find(a, b, 'D')) return;
    put_link(a, b, 'D', (double)time(NULL));
    save();
}
void srv_undecline(uint64_t a, uint64_t b) { drop(a, b, 'D'); }

typedef struct {
    uint64_t code;
    double since;
} Dated;

static int oldest_first(const void *x, const void *y) {
    double a = ((const Dated *)x)->since, b = ((const Dated *)y)->since;
    return a < b ? -1 : a > b;
}
static int newest_first(const void *x, const void *y) { return oldest_first(y, x); }

// what: 0 friends, 1 received, 2 sent
static int listed(uint64_t a, uint64_t *out, int max, int what) {
    Dated *d = malloc(((size_t)nlink + 1) * sizeof *d);
    if (!d) return 0;
    int n = 0;
    for (int i = 0; i < nlink; i++) {
        const Link *l = &links[i];
        if (l->kind != 'A') continue;
        if (what == 0 && l->a == a && srv_added(l->b, a)) {
            double back = srv_added_since(l->b, a);
            d[n++] = (Dated){l->b, l->since > back ? l->since : back};   // friends since the later of the two adds
        } else if (what == 1 && l->b == a && !srv_added(a, l->a)) {
            d[n++] = (Dated){l->a, l->since};
        } else if (what == 2 && l->a == a && !srv_added(l->b, a)) {
            d[n++] = (Dated){l->b, l->since};
        }
    }
    qsort(d, (size_t)n, sizeof *d, what == 0 ? oldest_first : newest_first);
    if (n > max) n = max;
    for (int i = 0; i < n; i++) out[i] = d[i].code;
    free(d);
    return n;
}

int srv_friends(uint64_t a, uint64_t *out, int max) { return listed(a, out, max, 0); }
int srv_received(uint64_t a, uint64_t *out, int max) { return listed(a, out, max, 1); }
int srv_sent(uint64_t a, uint64_t *out, int max) { return listed(a, out, max, 2); }

const char *srv_setting(const char *key, const char *fallback) {
    for (int i = 0; i < nset; i++)
        if (!strcmp(sets[i].key, key)) return sets[i].val;
    return fallback;
}

int srv_setting_int(const char *key, int fallback, int lo, int hi) {
    const char *v = srv_setting(key, NULL);
    int n = v ? atoi(v) : fallback;
    return n < lo ? lo : n > hi ? hi : n;
}
