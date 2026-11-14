// This PC's networks and its own online server, started by mkw (net_online.h).
//
// The server is built into the executable (runtime/srv): `mkw --server`, with its data (keys, accounts, logs) in
// server/ next to mkw. MKW_SERVER_HOST=1 starts it with the start screen, or with the game when it starts without one.
// It runs in a job object of the process that started it, which kills it when that process ends: the start screen's
// process waits for the game it starts (plat_reexec), so the server stops when the game, or the start screen, closes.
//
// MKW_NETWORK picks the networks: auto (Tailscale and the home network), home (the box's network: the adapter with a
// gateway) or tailscale (100.64.0.0/10). The server listens on 127.0.0.1 and the chosen ones, never on a public
// address; this PC plays it at the chosen network's address (auto: Tailscale's when it is up), the one its friends
// write, so the address the game reports to the others is one they reach.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <shellapi.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cfg.h"
#include "menu.h"
#include "net_online.h"
#include "ov.h"
#include "platform/platform.h"
#include "ui_text.h"

void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void sock_init(void) {
#ifdef _WIN32
    static volatile int done;
    if (!done) {
        WSADATA w;
        WSAStartup(MAKEWORD(2, 2), &w);
        done = 1;
    }
#endif
}

static int is_private(uint32_t h) { return (h >> 24) == 10 || (h >> 20) == 0xac1 || (h >> 16) == 0xc0a8; }
static int is_tailscale(uint32_t h) { return (h >> 22) == (100u << 2 | 1); }
static void fmt_ip(char out[16], uint32_t h) {
    snprintf(out, 16, "%u.%u.%u.%u", h >> 24, (h >> 16) & 255, (h >> 8) & 255, h & 255);
}

// Every IPv4 address of an adapter that is up (host order), with whether its adapter has a gateway and its metric.
typedef void (*AddrFn)(uint32_t h, int gateway, unsigned metric, int prefix, void *ctx);
static void each_address(AddrFn fn, void *ctx) {
#ifdef _WIN32
    ULONG flags = GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG size = 32 << 10, r = ERROR_BUFFER_OVERFLOW;
    IP_ADAPTER_ADDRESSES *list = NULL;
    for (int tries = 0; tries < 3 && r == ERROR_BUFFER_OVERFLOW; tries++) {
        free(list);
        list = malloc(size);
        r = list ? GetAdaptersAddresses(AF_INET, flags, NULL, list, &size) : ERROR_NOT_ENOUGH_MEMORY;
    }
    for (IP_ADAPTER_ADDRESSES *i = r == NO_ERROR ? list : NULL; i; i = i->Next) {
        if (i->OperStatus != IfOperStatusUp || i->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (IP_ADAPTER_UNICAST_ADDRESS *u = i->FirstUnicastAddress; u; u = u->Next)
            fn(ntohl(((struct sockaddr_in *)u->Address.lpSockaddr)->sin_addr.s_addr), i->FirstGatewayAddress != NULL,
               (unsigned)i->Ipv4Metric, u->OnLinkPrefixLength, ctx);
    }
    free(list);
#else
    struct ifaddrs *list, *i;
    if (getifaddrs(&list)) return;
    for (i = list; i; i = i->ifa_next) {
        if (!i->ifa_addr || !i->ifa_netmask || i->ifa_addr->sa_family != AF_INET || !(i->ifa_flags & IFF_UP) ||
            (i->ifa_flags & IFF_LOOPBACK))
            continue;
        uint32_t m = ntohl(((struct sockaddr_in *)i->ifa_netmask)->sin_addr.s_addr);
        fn(ntohl(((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr), m != 0xffffffffu, 0,
           m == 0xffffffffu ? 32 : 24, ctx);   // no gateway list here: a real subnet stands for one
    }
    freeifaddrs(list);
#endif
}

typedef struct { NetAddrs *a; unsigned best; } PickCtx;
static void pick(uint32_t h, int gateway, unsigned metric, int prefix, void *ctx) {
    PickCtx *p = ctx;
    if (is_tailscale(h)) {
        if (!p->a->ts[0]) fmt_ip(p->a->ts, h);
    } else if (gateway && prefix < 32 && is_private(h) && metric < p->best) {   // the box's network: the default route
        p->best = metric;
        fmt_ip(p->a->home, h);
    }
}

void net_addrs(NetAddrs *a) {
    memset(a, 0, sizeof *a);
    PickCtx p = {a, ~0u};
    each_address(pick, &p);
}

const char *net_network(void) {
    const char *v = cfg_get("MKW_NETWORK");
    return v && (!strcmp(v, "home") || !strcmp(v, "tailscale")) ? v : "auto";
}

// The chosen network's address of this PC; auto: Tailscale's when it is up, else the home network's. "" when none.
const char *net_network_address(const NetAddrs *a) {
    const char *n = net_network();
    if (!strcmp(n, "home")) return a->home;
    if (!strcmp(n, "tailscale")) return a->ts;
    return a->ts[0] ? a->ts : a->home;
}

typedef struct { uint32_t want; int found; } LocalCtx;
static void match(uint32_t h, int gateway, unsigned metric, int prefix, void *ctx) {
    (void)gateway, (void)metric, (void)prefix;
    LocalCtx *l = ctx;
    if (h == l->want) l->found = 1;
}

int net_is_this_pc(uint32_t ip) {
    uint32_t h = ntohl(ip);
    if ((h >> 24) == 127) return 1;
    LocalCtx l = {h, 0};
    each_address(match, &l);
    return l.found;
}

// ---- this PC: a hash of the system's own machine id (Windows' MachineGuid, Linux's /etc/machine-id), never the id
// itself. A friend code or a server's keys made on another PC (a copied folder) are told apart with it.
static uint64_t fnv1a(uint64_t h, const char *s) {
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 0x100000001b3ull;
    return h;
}

static char machine_id[17];

const char *net_machine_id(void) {
    if (machine_id[0]) return machine_id;
    char raw[128] = "";
#ifdef _WIN32
    wchar_t w[64];
    DWORD size = sizeof w;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Cryptography", L"MachineGuid",
                     RRF_RT_REG_SZ, NULL, w, &size) == ERROR_SUCCESS)
        WideCharToMultiByte(CP_UTF8, 0, w, -1, raw, sizeof raw, NULL, NULL);
#else
    uint64_t size = 0;
    char *t = (char *)plat_file_read("/etc/machine-id", &size);
    if (t) snprintf(raw, sizeof raw, "%.*s", (int)(size < 127 ? size : 127), t), free(t);
#endif
    // FNV-1a, salted: the id cannot be read back from it
    snprintf(machine_id, sizeof machine_id, "%016llx",
             (unsigned long long)fnv1a(fnv1a(0xcbf29ce484222325ull, "mkw-recomp/machine-id/"), raw));
    if (!raw[0]) rt_log("net: no machine id: copied folders cannot be told apart");
    return machine_id;
}

// ---- the server: built in, its data in server/ next to mkw
int net_host_enabled(void) { return cfg_bool("MKW_SERVER_HOST"); }

int net_host_available(void) {
#ifdef _WIN32
    return 1;   // built in
#else
    return 0;   // starting it from the start screen is in the Windows build for now (mkw --server runs anywhere)
#endif
}

// 127.0.0.1:8443 accepts a connection (150 ms at most): a server runs on this PC
static int port_answers(void) {
    sock_init();
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(NET_HOST_PORT)};
    to.sin_addr.s_addr = htonl(0x7f000001);
    int ok = 0;
#ifdef _WIN32
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return 0;
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    connect(s, (struct sockaddr *)&to, sizeof to);
    fd_set w, e;
    FD_ZERO(&w);
    FD_ZERO(&e);
    FD_SET(s, &w);
    FD_SET(s, &e);
    struct timeval tv = {0, 150000};
    if (select(0, NULL, &w, &e, &tv) > 0 && FD_ISSET(s, &w)) ok = 1;
    closesocket(s);
#else
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return 0;
    fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
    connect(s, (struct sockaddr *)&to, sizeof to);
    struct pollfd p = {s, POLLOUT, 0};
    int err = 0;
    socklen_t el = sizeof err;
    if (poll(&p, 1, 150) == 1 && !getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el) && !err) ok = 1;
    close(s);
#endif
    return ok;
}

#ifdef _WIN32
// The server's folder (server/ next to mkw) and other files beside the executable
static int beside_exe(const char *name, char *out, size_t cap) {
    char exe[1024];
    int n = plat_exe_path(exe, sizeof exe);
    char *cut = n > 0 && n < (int)sizeof exe ? plat_last_sep(exe) : NULL;
    if (!cut) return 0;
    *cut = 0;
    snprintf(out, cap, "%s/%s", exe, name);
    return 1;
}

// The addresses the server listens on for the network choice: 127.0.0.1, then the chosen networks' that are up
static void listen_list(char *out, size_t cap) {
    NetAddrs a;
    net_addrs(&a);
    const char *n = net_network();
    snprintf(out, cap, "127.0.0.1");
    if (a.home[0] && strcmp(n, "tailscale")) snprintf(out + strlen(out), cap - strlen(out), ",%s", a.home);
    if (a.ts[0] && strcmp(n, "home")) snprintf(out + strlen(out), cap - strlen(out), ",%s", a.ts);
}

static PlatMutex host_lock = PLAT_MUTEX_INIT;
static HANDLE host_job, host_proc;
static char host_listen[128];
static char host_error[200];

static wchar_t *wide(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = n > 0 ? malloc((size_t)n * sizeof *w) : NULL;
    if (w) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

static HANDLE open_inheritable(const char *path, int write) {
    wchar_t *w = wide(path);
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE h = w ? CreateFileW(w, write ? GENERIC_WRITE : GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                               write ? CREATE_ALWAYS : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL)
                 : INVALID_HANDLE_VALUE;
    free(w);
    return h;
}

static char host_console[1300];   // the running server's error output (why it stopped)

// The last line of the server's error output: why it stopped
static void last_error_line(const char *path, char *out, size_t cap) {
    uint64_t size = 0;
    char *t = (char *)plat_file_read(path, &size);
    out[0] = 0;
    if (!t) return;
    while (size && (t[size - 1] == '\n' || t[size - 1] == '\r' || t[size - 1] == ' ')) size--;
    size_t start = size;
    while (start && t[start - 1] != '\n') start--;
    snprintf(out, cap, "%.*s", (int)(size - start), t + start);
    free(t);
}

static void host_close_handles(void) {
    if (host_proc) CloseHandle(host_proc);
    if (host_job) CloseHandle(host_job);   // KILL_ON_JOB_CLOSE: ends the server if it still runs
    host_proc = host_job = NULL;
}

// The server's command line and folder (mkw --server, server/ next to mkw). 0 with err set when it cannot run here.
static int server_command(const char *hosts, char *cmd, size_t ccap, char *exe, size_t ecap, char *dir, size_t dcap,
                          char *logs, size_t lcap, char *err, size_t cap) {
    if (plat_exe_path(exe, ecap) <= 0 || !beside_exe("server", dir, dcap)) {
        snprintf(err, cap, "this program's own path is unknown");
        return 0;
    }
    snprintf(logs, lcap, "%s/logs", dir);
    int n = snprintf(cmd, ccap, "\"%s\" --server --port %d --data \"%s\"", exe, NET_HOST_PORT, dir);
    for (const char *h = hosts; *h && n > 0 && (size_t)n < ccap;) {   // "a,b,c": --host a --host b --host c
        size_t len = strcspn(h, ",");
        n += snprintf(cmd + n, ccap - (size_t)n, " --host %.*s", (int)len, h);
        h += len + (h[len] == ',');
    }
    return n > 0 && (size_t)n < ccap;
}

int net_host_start(char *err, size_t cap) {
    char dir[1100], exe[1100], logs[1200], out_path[1300], hosts[128], cmd[4096];
    err[0] = 0;
    if (!beside_exe("server", dir, sizeof dir)) {
        snprintf(err, cap, "this program's own path is unknown");
        return -1;
    }
    // The server's keys and accounts belong to the PC that made them: a folder copied from another PC is set aside,
    // never used (its private key is that PC's identity), and this PC's server makes its own keys at its first start.
    char data[1150], mark[1200];
    snprintf(data, sizeof data, "%s", dir);
    snprintf(mark, sizeof mark, "%s/machine-id", data);
    uint64_t msize = 0;
    char *m = (char *)plat_file_read(mark, &msize);
    int marked = m != NULL, foreign = marked && (msize < 16 || memcmp(m, net_machine_id(), 16));
    free(m);
    if (foreign) {
        char aside[1250];
        snprintf(aside, sizeof aside, "%s.autre-pc-%llu", data, (unsigned long long)time(NULL));
        if (!plat_file_replace(data, aside)) {
            snprintf(err, cap, "%s was made on another PC and could not be set aside", data);
            return -1;
        }
        rt_log("net: %s was made on another PC: set aside as %s, this PC's server starts anew", data, aside);
    }
    if (!marked || foreign) {
        plat_file_mkdir_p(data);
        plat_file_write(mark, net_machine_id(), 16);
    }
    plat_mutex_lock(&host_lock);
    if (host_proc && WaitForSingleObject(host_proc, 0) == WAIT_TIMEOUT) {   // ours runs already
        plat_mutex_unlock(&host_lock);
        return 0;
    }
    host_close_handles();
    if (port_answers()) {
        plat_mutex_unlock(&host_lock);
        snprintf(err, cap, "another server already answers on port %d of this PC", NET_HOST_PORT);
        return -1;
    }
    listen_list(hosts, sizeof hosts);
    if (!server_command(hosts, cmd, sizeof cmd, exe, sizeof exe, dir, sizeof dir, logs, sizeof logs, err, cap)) {
        plat_mutex_unlock(&host_lock);
        if (!err[0]) snprintf(err, cap, "the server's command line is too long");
        return -1;
    }
    plat_file_mkdir_p(logs);
    snprintf(out_path, sizeof out_path, "%s/console-sortie.txt", logs);
    snprintf(host_console, sizeof host_console, "%s/console-erreurs.txt", logs);
    HANDLE in = open_inheritable("NUL", 0), so = open_inheritable(out_path, 1), se = open_inheritable(host_console, 1);
    HANDLE job = CreateJobObjectW(NULL, NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {0};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (job) SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
    // Only the three standard handles are inherited, none of mkw's own files
    HANDLE inherit[3] = {in, so, se};
    SIZE_T asize = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &asize);
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = malloc(asize);
    int ok = attrs && InitializeProcThreadAttributeList(attrs, 1, 0, &asize) &&
             UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof inherit, NULL, NULL);
    STARTUPINFOEXW si = {0};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = in, si.StartupInfo.hStdOutput = so, si.StartupInfo.hStdError = se;
    si.lpAttributeList = attrs;
    PROCESS_INFORMATION pi = {0};
    wchar_t *wcmd = wide(cmd), *wdir = wide(dir), *wexe = wide(exe);
    ok = ok && in != INVALID_HANDLE_VALUE && so != INVALID_HANDLE_VALUE && se != INVALID_HANDLE_VALUE && wcmd &&
         wdir && wexe &&
         CreateProcessW(wexe, wcmd, NULL, NULL, TRUE,
                        CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, NULL, wdir,
                        &si.StartupInfo, &pi);
    DWORD why = ok ? 0 : GetLastError();
    const char *name = "online server";
    if (ok) {
        if (!job || !AssignProcessToJobObject(job, pi.hProcess))
            rt_log("net: %s not tied to mkw's lifetime (job error %lu): it outlives mkw", name, GetLastError());
        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);
        host_proc = pi.hProcess, host_job = job;
        snprintf(host_listen, sizeof host_listen, "%s", hosts);
        host_error[0] = 0;
        plat_setenv("MKW_HOST_STARTED", "1");   // the game this start screen runs finds it started
        rt_log("net: online server started on %s port %d (pid %lu), stops with mkw", hosts, NET_HOST_PORT,
               pi.dwProcessId);
    } else {
        if (job) CloseHandle(job);
        snprintf(err, cap, "Windows did not start the %s (error %lu)", name, why);
        rt_log("net: %s", err);
    }
    free(wcmd), free(wdir), free(wexe);
    if (attrs) DeleteProcThreadAttributeList(attrs), free(attrs);
    for (int i = 0; i < 3; i++)
        if (inherit[i] != INVALID_HANDLE_VALUE) CloseHandle(inherit[i]);
    plat_mutex_unlock(&host_lock);
    return ok ? 0 : -1;
}

void net_host_stop(void) {
    plat_mutex_lock(&host_lock);
    if (host_job) TerminateJobObject(host_job, 0);
    else if (host_proc) TerminateProcess(host_proc, 0);
    if (host_proc) {
        WaitForSingleObject(host_proc, 3000);
        rt_log("net: the server stopped");
    }
    host_close_handles();
    host_listen[0] = 0;
    plat_mutex_unlock(&host_lock);
}

NetHostState net_host_state(char *detail, size_t cap) {
    if (detail && cap) detail[0] = 0;
    plat_mutex_lock(&host_lock);
    NetHostState st;
    if (host_proc && WaitForSingleObject(host_proc, 0) == WAIT_OBJECT_0) {   // ours ended
        DWORD code = 0;
        GetExitCodeProcess(host_proc, &code);
        last_error_line(host_console, host_error, sizeof host_error);
        if (!host_error[0]) snprintf(host_error, sizeof host_error, "the server stopped (code %lu)", code);
        rt_log("net: the server ended: %s", host_error);
        host_close_handles();
    }
    if (host_proc) {
        st = port_answers() ? NET_HOST_RUNNING : NET_HOST_STARTING;
        if (detail && cap) snprintf(detail, cap, "%s", host_listen);
    } else if (port_answers()) {
        st = NET_HOST_OTHER;
    } else if (host_error[0]) {
        st = NET_HOST_FAILED;
        if (detail && cap) snprintf(detail, cap, "%s", host_error);
    } else {
        st = NET_HOST_OFF;
    }
    plat_mutex_unlock(&host_lock);
    return st;
}

int net_host_listen_changed(void) {
    char now[128];
    listen_list(now, sizeof now);
    plat_mutex_lock(&host_lock);
    int changed = host_proc && strcmp(now, host_listen) != 0;
    plat_mutex_unlock(&host_lock);
    return changed;
}

// Windows' firewall, once per PC (Windows asks first, "runas"): the built-in server's program may take connections
// from the local network and from Tailscale (100.64.0.0/10), never from the Internet.
int net_host_firewall(void) {
    char exe[1024], args[2600], quoted[2100];
    int n = plat_exe_path(exe, sizeof exe);
    if (n <= 0 || n >= (int)sizeof exe) return -1;
    size_t q = 0;   // the path in a PowerShell single-quoted string: ' doubled
    for (const char *s = exe; *s && q + 2 < sizeof quoted; s++) {
        if (*s == '\'') quoted[q++] = '\'';
        quoted[q++] = *s;
    }
    quoted[q] = 0;
    snprintf(args, sizeof args,
             "-NoProfile -Command \"Remove-NetFirewallRule -DisplayName 'MKW Recomp server' -ErrorAction "
             "SilentlyContinue; New-NetFirewallRule -DisplayName 'MKW Recomp server' -Direction Inbound -Action "
             "Allow -Program '%s' -RemoteAddress LocalSubnet,100.64.0.0/10 -Profile Any | Out-Null\"",
             quoted);
    wchar_t *w = wide(args);
    // "runas": Windows asks the player (UAC) before the firewall changes
    HINSTANCE r = w ? ShellExecuteW(NULL, L"runas", L"powershell.exe", w, NULL, SW_HIDE) : NULL;
    free(w);
    return (INT_PTR)r > 32 ? 0 : -1;
}

#else
int net_host_start(char *err, size_t cap) {
    snprintf(err, cap, "hosting the server is in the Windows build only");
    return -1;
}
void net_host_stop(void) {}
NetHostState net_host_state(char *detail, size_t cap) {
    if (detail && cap) detail[0] = 0;
    return port_answers() ? NET_HOST_OTHER : NET_HOST_OFF;
}
int net_host_listen_changed(void) { return 0; }
int net_host_firewall(void) { return -1; }
#endif

// A network error the console shows in a dialog the player closes (nifm's network applet, the error applet): the
// runtime shows no applet, so the game's own line flashes for a second as it goes back to its menu. A notice at the
// top of the picture says it for 10 seconds instead, with what to check. category 0: no network for the request.
void net_error_notice(unsigned category, unsigned number) {
    static PlatMutex lock = PLAT_MUTEX_INIT;
    static uint64_t last_ms;
    static unsigned last_key = ~0u;
    unsigned key = category << 16 ^ number;
    uint64_t now = plat_now_ms();
    plat_mutex_lock(&lock);
    int again = key == last_key && now - last_ms < 5000;   // the same error retried at once: said once
    last_key = key, last_ms = now;
    plat_mutex_unlock(&lock);
    if (again) return;
    char why[200], text[256];
    const NetServer *s = net_server();
    if (!net_enabled()) snprintf(why, sizeof why, "%s", menu_text(UI_NOTICE_OFFLINE));
    else if (!s->on) snprintf(why, sizeof why, "%s", menu_text(UI_NOTICE_NO_SERVER));
    else snprintf(why, sizeof why, menu_text(UI_NOTICE_SERVER), s->text);
    if (category) {
        char code[16];
        snprintf(code, sizeof code, "%04u-%04u", category, number);
        snprintf(text, sizeof text, menu_text(UI_NOTICE_ERROR), code, why);
    } else {
        snprintf(text, sizeof text, menu_text(UI_NOTICE_NO_NETWORK), why);
    }
    rt_log("net: notice: %s", text);
    menu_notice(text, 10);
}

void net_host_boot(void) {
    const char *started = getenv("MKW_HOST_STARTED");
    if (!net_host_enabled() || (started && *started) || !net_host_available()) return;
    char err[200];
    if (net_host_start(err, sizeof err)) rt_log("net: the server did not start: %s", err);
}
