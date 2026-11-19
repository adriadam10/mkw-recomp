// Windows implementation of platform.h, machine load and the desktop shortcut (the parts: win32_int.h).
#include <io.h>
#include <malloc.h>
#include <stdio.h>
#include <windows.h>
#define COBJMACROS
#include <dxgi.h>
#include <bcrypt.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <timeapi.h>
#include <tlhelp32.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

#include "platform.h"

#include "win32_int.h"

// ---- machine load: GetSystemTimes for the CPU, PDH (pdh.dll, loaded on first use) for the thermal zones and the GPU
// engine and adapter memory counters that Task Manager reads.

typedef PDH_STATUS(WINAPI *PdhOpenQueryW_t)(LPCWSTR, DWORD_PTR, PDH_HQUERY *);
typedef PDH_STATUS(WINAPI *PdhAddEnglishCounterW_t)(PDH_HQUERY, LPCWSTR, DWORD_PTR, PDH_HCOUNTER *);
typedef PDH_STATUS(WINAPI *PdhCollectQueryData_t)(PDH_HQUERY);
typedef PDH_STATUS(WINAPI *PdhGetFormattedCounterArrayW_t)(PDH_HCOUNTER, DWORD, LPDWORD, LPDWORD,
                                                           PPDH_FMT_COUNTERVALUE_ITEM_W);
typedef PDH_STATUS(WINAPI *PdhCloseQuery_t)(PDH_HQUERY);
static struct {
    INIT_ONCE once;
    PdhOpenQueryW_t open;
    PdhAddEnglishCounterW_t add;
    PdhCollectQueryData_t collect;
    PdhGetFormattedCounterArrayW_t get;
    PdhCloseQuery_t close;
} pdh = {INIT_ONCE_STATIC_INIT};

static BOOL CALLBACK pdh_load(INIT_ONCE *o, void *p, void **c) {
    (void)o, (void)p, (void)c;
    HMODULE l = plat_lib_open("pdh.dll");
    if (!l) return TRUE;
    pdh.add = (PdhAddEnglishCounterW_t)(void *)GetProcAddress(l, "PdhAddEnglishCounterW");
    pdh.collect = (PdhCollectQueryData_t)(void *)GetProcAddress(l, "PdhCollectQueryData");
    pdh.get = (PdhGetFormattedCounterArrayW_t)(void *)GetProcAddress(l, "PdhGetFormattedCounterArrayW");
    pdh.close = (PdhCloseQuery_t)(void *)GetProcAddress(l, "PdhCloseQuery");
    PdhOpenQueryW_t open = (PdhOpenQueryW_t)(void *)GetProcAddress(l, "PdhOpenQueryW");
    if (pdh.add && pdh.collect && pdh.get && pdh.close) pdh.open = open;   // open set: all of them are there
    return TRUE;
}

// One PDH query with its wildcard counters ("\Object(*)\Counter"). A query whose open failed stays NULL.
typedef struct { PDH_HQUERY q; PDH_HCOUNTER c[2]; } PdhQuery;

static void pdh_query(PdhQuery *pq, const wchar_t *c0, const wchar_t *c1) {
    InitOnceExecuteOnce(&pdh.once, pdh_load, NULL, NULL);
    pq->q = NULL;
    if (!pdh.open || pdh.open(NULL, 0, &pq->q) != ERROR_SUCCESS) {
        pq->q = NULL;
        return;
    }
    if (pdh.add(pq->q, c0, 0, &pq->c[0]) != ERROR_SUCCESS) pq->c[0] = NULL;
    if (!c1 || pdh.add(pq->q, c1, 0, &pq->c[1]) != ERROR_SUCCESS) pq->c[1] = NULL;
    if (!pq->c[0] && !pq->c[1]) {
        pdh.close(pq->q);
        pq->q = NULL;
    }
}

// The counter's instances as a malloc'd array (free it), *count of them; NULL when there is no valid data yet (a rate
// counter needs two collections).
static PDH_FMT_COUNTERVALUE_ITEM_W *pdh_items(PDH_HCOUNTER c, DWORD *count) {
    DWORD size = 0;
    *count = 0;
    if (!c || pdh.get(c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, count, NULL) != PDH_MORE_DATA || !size) return NULL;
    PDH_FMT_COUNTERVALUE_ITEM_W *items = malloc(size);
    if (items && pdh.get(c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, count, items) == ERROR_SUCCESS) return items;
    free(items);
    *count = 0;
    return NULL;
}

struct PlatLoad {
    uint64_t idle, kernel, user, proc;   // the previous call's times; kernel 0 before the first
    PdhQuery thermal, gpu;               // gpu: engine utilisation, adapter dedicated memory
    wchar_t luid[40];                    // "luid_0x<high>_0x<low>" of the adapter, lower case; empty for any
};

uint64_t plat_gpu_vram_mib(uint32_t vendor, uint32_t device, const uint8_t *luid) {
    (void)vendor, (void)device;
    typedef HRESULT(WINAPI * CreateFactory_t)(REFIID, void **);
    static const GUID iid_factory1 = {0x770aae78, 0xf26f, 0x4dba, {0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87}};
    HMODULE l = luid ? plat_lib_open("dxgi.dll") : NULL;
    CreateFactory_t create = l ? (CreateFactory_t)(void *)GetProcAddress(l, "CreateDXGIFactory1") : NULL;
    IDXGIFactory1 *f;
    if (!create || FAILED(create(&iid_factory1, (void **)&f))) return 0;
    uint64_t mib = 0;
    IDXGIAdapter1 *a;
    for (UINT i = 0; IDXGIFactory1_EnumAdapters1(f, i, &a) == S_OK; i++) {
        DXGI_ADAPTER_DESC1 desc;
        if (SUCCEEDED(IDXGIAdapter1_GetDesc1(a, &desc)) && !memcmp(&desc.AdapterLuid, luid, 8))
            mib = desc.DedicatedVideoMemory >> 20;
        IDXGIAdapter1_Release(a);
    }
    IDXGIFactory1_Release(f);
    return mib;
}

PlatLoad *plat_load_open(const uint8_t *gpu_luid) {
    PlatLoad *l = calloc(1, sizeof *l);
    if (!l) return NULL;
    if (gpu_luid) {   // Vulkan's deviceLUID is the LUID struct's memory: LowPart, then HighPart
        uint32_t lo, hi;
        memcpy(&lo, gpu_luid, 4);
        memcpy(&hi, gpu_luid + 4, 4);
        swprintf(l->luid, sizeof l->luid / sizeof *l->luid, L"luid_0x%08x_0x%08x", hi, lo);
    }
    pdh_query(&l->thermal, L"\\Thermal Zone Information(*)\\Temperature", NULL);
    pdh_query(&l->gpu, L"\\GPU Engine(*)\\Utilization Percentage", L"\\GPU Adapter Memory(*)\\Dedicated Usage");
    return l;
}

void plat_load_close(PlatLoad *l) {
    if (!l) return;
    if (l->thermal.q) pdh.close(l->thermal.q);
    if (l->gpu.q) pdh.close(l->gpu.q);
    free(l);
}

void plat_cpu_load(PlatLoad *l, float *total_pct, float *proc_pct, float *temp_c) {
    *total_pct = *proc_pct = *temp_c = -1;
    if (!l) return;
    FILETIME idle, kern, user, cr, ex, pk, pu;
    if (GetSystemTimes(&idle, &kern, &user) && GetProcessTimes(GetCurrentProcess(), &cr, &ex, &pk, &pu)) {
        uint64_t i = ft64(idle), k = ft64(kern), u = ft64(user), p = ft64(pk) + ft64(pu);
        uint64_t total = (k - l->kernel) + (u - l->user);   // kernel time includes idle time
        if (l->kernel && total) {
            *total_pct = 100.0f * (float)(total - (i - l->idle)) / (float)total;
            *proc_pct = 100.0f * (float)(p - l->proc) / (float)total;
        }
        l->idle = i, l->kernel = k, l->user = u, l->proc = p;
    }
    // the firmware's ACPI zones, near the CPU on most boards; the exact package temperature needs a kernel driver
    DWORD n;
    PDH_FMT_COUNTERVALUE_ITEM_W *it;
    if (l->thermal.q && pdh.collect(l->thermal.q) == ERROR_SUCCESS && (it = pdh_items(l->thermal.c[0], &n))) {
        for (DWORD i = 0; i < n; i++) {   // the hottest zone, in kelvin
            double c = it[i].FmtValue.doubleValue - 273.15;
            if (it[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA && c > *temp_c && c < 150) *temp_c = (float)c;
        }
        free(it);
    }
}

// The adapter part of an instance name, "luid_0x..._0x...", lower case, into out (40 wide characters).
static void inst_luid(const wchar_t *name, wchar_t *out) {
    const wchar_t *p = wcsstr(name, L"luid_");
    size_t n = 0;
    while (p && p[n] && n < 26 && n < 39) out[n] = (wchar_t)towlower(p[n]), n++;   // luid_0x%08x_0x%08x: 26
    out[n] = 0;
}

void plat_gpu_load(PlatLoad *l, float *load_pct, float *vram_used_mb) {
    *load_pct = *vram_used_mb = -1;
    if (!l || !l->gpu.q || pdh.collect(l->gpu.q) != ERROR_SUCCESS) return;
    // Engine instances: "pid_<n>_luid_0x.._0x.._phys_<n>_eng_<n>_engtype_<type>". Per adapter and engine type the
    // processes' shares add up; the load is the busiest engine type.
    enum { TYPES = 32 };
    struct { wchar_t luid[40], type[32]; double sum; } t[TYPES];
    int nt = 0;
    DWORD n;
    PDH_FMT_COUNTERVALUE_ITEM_W *it = pdh_items(l->gpu.c[0], &n);
    for (DWORD i = 0; it && i < n; i++) {
        if (it[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA) continue;
        wchar_t lu[40];
        inst_luid(it[i].szName, lu);
        if (l->luid[0] && wcscmp(lu, l->luid)) continue;
        const wchar_t *ty = wcsstr(it[i].szName, L"engtype_");
        ty = ty ? ty + 8 : L"";
        int k = 0;
        while (k < nt && (wcscmp(t[k].luid, lu) || wcsncmp(t[k].type, ty, 31))) k++;
        if (k == nt) {
            if (nt == TYPES) continue;
            wcscpy(t[k].luid, lu);
            wcsncpy(t[k].type, ty, 31);
            t[k].type[31] = 0;
            t[k].sum = 0;
            nt++;
        }
        t[k].sum += it[i].FmtValue.doubleValue;
    }
    free(it);
    for (int k = 0; k < nt; k++)
        if (t[k].sum > *load_pct) *load_pct = (float)(t[k].sum > 100 ? 100 : t[k].sum);
    // Adapter memory instances: "luid_0x.._0x.._phys_<n>", bytes. Without a LUID: the adapter that uses the most.
    it = pdh_items(l->gpu.c[1], &n);
    double sum = 0, best = -1;
    for (DWORD i = 0; it && i < n; i++) {
        if (it[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA) continue;
        wchar_t lu[40];
        inst_luid(it[i].szName, lu);
        if (l->luid[0] ? !wcscmp(lu, l->luid) : 0) sum += it[i].FmtValue.doubleValue;
        if (it[i].FmtValue.doubleValue > best) best = it[i].FmtValue.doubleValue;
    }
    if (it && (l->luid[0] ? sum > 0 : best >= 0)) *vram_used_mb = (float)((l->luid[0] ? sum : best) / 1048576.0);
    free(it);
}

void plat_mem_info(float *used_mb, float *total_mb, float *proc_mb) {
    *used_mb = *total_mb = *proc_mb = -1;
    MEMORYSTATUSEX ms = {.dwLength = sizeof ms};
    if (GlobalMemoryStatusEx(&ms)) {
        *total_mb = (float)(ms.ullTotalPhys >> 20);
        *used_mb = (float)((ms.ullTotalPhys - ms.ullAvailPhys) >> 20);
    }
    PROCESS_MEMORY_COUNTERS pmc;
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) *proc_mb = (float)(pmc.WorkingSetSize >> 20);
}

void plat_process_memory(uint64_t *resident, uint64_t *private_bytes) {
    PROCESS_MEMORY_COUNTERS_EX pm = {.cb = sizeof pm};
    *resident = *private_bytes = 0;
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pm, sizeof pm))
        *resident = pm.WorkingSetSize, *private_bytes = pm.PrivateUsage;
}

void plat_heap_trim(void) { _heapmin(); }

// Placed maps are Linux-only (vk_memory.c placed_pool): nothing to undo here.
int plat_mem_replace_anon(uint64_t addr, uint64_t n) { (void)addr, (void)n; return 0; }

// CPU time (user + kernel) of this process's threads with that description (SetThreadDescription in plat_thread_new_ex),
// summed; -1 when none. GetThreadTimes ticks at ~15.6 ms: the overlay sums seconds of it.
int64_t plat_threads_cpu_ns_named(const char *name) {
    wchar_t want[64];
    if (!MultiByteToWideChar(CP_UTF8, 0, name, -1, want, 64)) return -1;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return -1;
    DWORD pid = GetCurrentProcessId();
    int64_t total = -1;
    THREADENTRY32 te = {.dwSize = sizeof te};
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid) continue;
        HANDLE th = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
        if (!th) continue;
        PWSTR desc = NULL;
        FILETIME a, b, k, u;
        if (SUCCEEDED(GetThreadDescription(th, &desc)) && desc && !wcscmp(desc, want) && GetThreadTimes(th, &a, &b, &k, &u))
            total = (total < 0 ? 0 : total) + (int64_t)((((uint64_t)k.dwHighDateTime << 32 | k.dwLowDateTime) +
                                                         ((uint64_t)u.dwHighDateTime << 32 | u.dwLowDateTime)) * 100);
        if (desc) LocalFree(desc);
        CloseHandle(th);
    }
    CloseHandle(snap);
    return total;
}

// ---- desktop shortcut: IShellLinkW saved through its IPersistFile, on the user's Desktop (FOLDERID_Desktop, which
// follows a Desktop moved to OneDrive). Called on a thread COM may not be set up on: initialised and released here.
int plat_desktop_shortcut(const char *name, const char *exe, const char *dir, char *out, size_t cap) {
    wchar_t wexe[WPATH], wdir[WPATH], wname[256], link[WPATH];
    if (!widen(exe, wexe, WPATH) || !widen(dir, wdir, WPATH) || !widen(name, wname, 256)) return 0;
    PWSTR desk = NULL;
    if (FAILED(SHGetKnownFolderPath(&FOLDERID_Desktop, 0, NULL, &desk))) return 0;
    _snwprintf(link, WPATH, L"%ls\\%ls.lnk", desk, wname);
    link[WPATH - 1] = 0;
    CoTaskMemFree(desk);
    HRESULT init = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    IShellLinkW *sl = NULL;
    IPersistFile *pf = NULL;
    int ok = SUCCEEDED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&sl)) &&
             SUCCEEDED(sl->lpVtbl->SetPath(sl, wexe)) && SUCCEEDED(sl->lpVtbl->SetWorkingDirectory(sl, wdir)) &&
             SUCCEEDED(sl->lpVtbl->SetIconLocation(sl, wexe, 0)) &&
             SUCCEEDED(sl->lpVtbl->QueryInterface(sl, &IID_IPersistFile, (void **)&pf)) &&
             SUCCEEDED(pf->lpVtbl->Save(pf, link, TRUE));
    if (pf) pf->lpVtbl->Release(pf);
    if (sl) sl->lpVtbl->Release(sl);
    if (SUCCEEDED(init)) CoUninitialize();
    if (ok && out && cap) WideCharToMultiByte(CP_UTF8, 0, link, -1, out, (int)cap, NULL, NULL);
    return ok;
}

// ---- a URL scheme (GameBanana's 1-Click: "scheme:https://gamebanana.com/mmdl/...") opening exe with arg and the URL:
// HKCU\Software\Classes\<scheme>, the current user's own (no administrator), rewritten only when it differs.
int plat_url_scheme(const char *scheme, const char *label, const char *exe, const char *arg) {
    wchar_t wscheme[64], wlabel[128], wexe[WPATH], warg[64], key[160], cmd[WPATH + 160], have[WPATH + 160];
    if (!widen(scheme, wscheme, 64) || !widen(label, wlabel, 128) || !widen(exe, wexe, WPATH) || !widen(arg, warg, 64))
        return 0;
    _snwprintf(key, 160, L"Software\\Classes\\%ls\\shell\\open\\command", wscheme);
    _snwprintf(cmd, WPATH + 160, L"\"%ls\" %ls \"%%1\"", wexe, warg);
    DWORD n = sizeof have;
    if (RegGetValueW(HKEY_CURRENT_USER, key, NULL, RRF_RT_REG_SZ, NULL, have, &n) == ERROR_SUCCESS && !wcscmp(have, cmd))
        return 1;   // already ours, at this path
    wchar_t root[96];
    _snwprintf(root, 96, L"Software\\Classes\\%ls", wscheme);
    int ok = RegSetKeyValueW(HKEY_CURRENT_USER, root, NULL, REG_SZ, wlabel, (DWORD)((wcslen(wlabel) + 1) * 2)) == 0 &&
             RegSetKeyValueW(HKEY_CURRENT_USER, root, L"URL Protocol", REG_SZ, L"", 2) == 0 &&
             RegSetKeyValueW(HKEY_CURRENT_USER, key, NULL, REG_SZ, cmd, (DWORD)((wcslen(cmd) + 1) * 2)) == 0;
    return ok;
}
