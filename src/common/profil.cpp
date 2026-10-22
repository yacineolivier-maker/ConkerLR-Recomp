// Profileur par échantillonnage (diagnostic) : XBE_PROFILE=<début s>,<durée s> relève toutes les millisecondes
// l'adresse d'exécution de chaque thread du processus, puis journalise les fonctions les plus vues par thread
// (noms des symboles publics du PDB : les fonctions recompilées s'appellent sub_<adresse invitée>).
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core/log.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

#pragma comment(lib, "dbghelp.lib")

namespace game {

namespace {

std::string Symbol(HANDLE proc, uint64_t a) {
    alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256] = {};
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    DWORD64 disp = 0;
    if (SymFromAddr(proc, a, &disp, sym)) return sym->Name;
    HMODULE mod = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(a), &mod))
        GetModuleFileNameA(mod, name, MAX_PATH);
    std::string s = name;
    return s.substr(s.find_last_of('\\') + 1);
}

void Run(int start, int seconds) {
    std::this_thread::sleep_for(std::chrono::seconds(start));
    HANDLE proc = GetCurrentProcess();
    DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    std::map<DWORD, std::unordered_map<uint64_t, uint32_t>> hits;  // thread -> adresse -> échantillons
    std::map<DWORD, uint32_t> totals;
    // the word at the stack top: a leaf function's caller (memchr, the clock...), else noise
    std::map<DWORD, std::unordered_map<uint64_t, std::unordered_map<uint64_t, uint32_t>>> callers;
    // the threads opened once: enumerating them each millisecond left a few samples a second
    std::vector<std::pair<DWORD, HANDLE>> threads;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te{sizeof(te)};
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        if (HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID))
            threads.push_back({te.th32ThreadID, t});
    }
    CloseHandle(snap);
    auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < end) {
        for (auto& [tid, t] : threads) {
            if (SuspendThread(t) == DWORD(-1)) continue;
            CONTEXT c{};
            c.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(t, &c)) {
                hits[tid][c.Rip]++;
                callers[tid][c.Rip][*reinterpret_cast<const uint64_t*>(c.Rsp)]++;
                totals[tid]++;
            }
            ResumeThread(t);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    for (auto& [tid, t] : threads) CloseHandle(t);
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    SymInitialize(proc, nullptr, TRUE);
    for (auto& [tid, addrs] : hits) {
        uint32_t total = totals[tid];
        std::unordered_map<std::string, uint32_t> by_fn;
        for (auto& [a, n] : addrs) by_fn[Symbol(proc, a)] += n;
        std::vector<std::pair<uint32_t, std::string>> v;
        for (auto& [f, n] : by_fn) v.push_back({n, f});
        std::sort(v.rbegin(), v.rend());
        // les threads qui attendent presque toujours au même endroit n'apprennent rien
        if (v.empty()) continue;
        const std::string& top = v[0].second;
        if ((top.rfind("Zw", 0) == 0 || top.rfind("Nt", 0) == 0) && v[0].first * 20 >= total * 19) continue;
        LOGI("profil : thread %lu, %u échantillons", tid, total);
        for (size_t i = 0; i < v.size() && i < 25; i++)
            LOGI("profil :   %5.1f %%  %s", 100.0 * v[i].first / total, v[i].second.c_str());
        // the busiest functions of our own code line by line (line tables of the runtime and host code)
        std::unordered_map<std::string, std::unordered_map<std::string, uint32_t>> by_line;
        for (auto& [a, n] : addrs) {
            IMAGEHLP_LINE64 line{sizeof(line)};
            DWORD disp = 0;
            if (!SymGetLineFromAddr64(proc, a, &disp, &line)) continue;
            std::string file = line.FileName;
            by_line[Symbol(proc, a)][file.substr(file.find_last_of("\\/") + 1) + ":" + std::to_string(line.LineNumber)] += n;
        }
        for (size_t i = 0, shown = 0; i < v.size() && shown < 3; i++) {
            auto it = by_line.find(v[i].second);
            if (it == by_line.end()) continue;
            shown++;
            std::vector<std::pair<uint32_t, std::string>> w;
            for (auto& [l, n] : it->second) w.push_back({n, l});
            std::sort(w.rbegin(), w.rend());
            for (size_t k = 0; k < w.size() && k < 6; k++)
                LOGI("profil :     %s : %4.1f %%  %s", v[i].second.substr(0, 40).c_str(), 100.0 * w[k].first / total,
                     w[k].second.c_str());
        }
        // the callers of the busiest functions, by the return address at the stack top
        std::unordered_map<std::string, std::unordered_map<std::string, uint32_t>> by_caller;
        for (auto& [a, rets] : callers[tid])
            for (auto& [r, n] : rets) by_caller[Symbol(proc, a)][Symbol(proc, r)] += n;
        for (size_t i = 0; i < v.size() && i < 8; i++) {
            std::vector<std::pair<uint32_t, std::string>> w;
            for (auto& [f, n] : by_caller[v[i].second]) w.push_back({n, f});
            std::sort(w.rbegin(), w.rend());
            for (size_t k = 0; k < w.size() && k < 3; k++)
                LOGI("profil :     %s <- %4.1f %%  %s", v[i].second.substr(0, 40).c_str(), 100.0 * w[k].first / total,
                     w[k].second.c_str());
        }
    }
    logx::Flush();
}

// XBE_WRITEWATCH=<adresse>|*<adresse>+<décalage>[@<secondes>] (hexadécimal) : la page du mot protégée en écriture
// (après ce délai si donné) ; chaque
// instruction qui écrit ce mot est journalisée une fois, avec ses appelants (fonctions recompilées sub_<adresse>)
struct WriteWatch {
    uint8_t* word = nullptr;
    uint8_t* page = nullptr;
    struct Hit { uint64_t frames[8]; uint32_t n; };
    Hit hits[256];
    volatile long count = 0;
};
WriteWatch g_ww;
thread_local bool t_stepping = false;

LONG CALLBACK WriteWatchHandler(EXCEPTION_POINTERS* e) {
    const EXCEPTION_RECORD& r = *e->ExceptionRecord;
    if (r.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r.NumberParameters >= 2 && r.ExceptionInformation[0] == 1) {
        auto* at = reinterpret_cast<uint8_t*>(r.ExceptionInformation[1]);
        if (at < g_ww.page || at >= g_ww.page + 0x1000) return EXCEPTION_CONTINUE_SEARCH;
        if (at + 8 > g_ww.word && at < g_ww.word + 4) {
            long i = InterlockedIncrement(&g_ww.count) - 1;
            if (i < 256) {
                WriteWatch::Hit& h = g_ww.hits[i];
                h.frames[0] = e->ContextRecord->Rip;
                h.n = 1 + RtlCaptureStackBackTrace(0, 7, reinterpret_cast<void**>(&h.frames[1]), nullptr);
            }
        }
        DWORD old;
        VirtualProtect(g_ww.page, 0x1000, PAGE_READWRITE, &old);
        e->ContextRecord->EFlags |= 0x100;  // une instruction, puis la page protégée de nouveau
        t_stepping = true;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (r.ExceptionCode == EXCEPTION_SINGLE_STEP && t_stepping) {
        t_stepping = false;
        DWORD old;
        VirtualProtect(g_ww.page, 0x1000, PAGE_READONLY, &old);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void RunWriteWatch(std::string expr) {
    if (size_t at = expr.find('@'); at != std::string::npos)
        std::this_thread::sleep_for(std::chrono::seconds(std::atoi(expr.c_str() + at + 1)));
    bool deref = expr[0] == '*';
    char* q = nullptr;
    uint32_t addr = uint32_t(std::strtoul(expr.c_str() + (deref ? 1 : 0), &q, 16)), off = 0;
    if (q && *q == '+') off = uint32_t(std::strtoul(q + 1, nullptr, 16));
    for (;;) {
        if (!deref) break;
        if (mem::IsCommitted(addr) && kernel::R32(addr)) {
            addr = kernel::R32(addr) + off;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    g_ww.word = static_cast<uint8_t*>(kernel::HostPtr(addr));
    g_ww.page = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(g_ww.word) & ~uintptr_t(0xFFF));
    AddVectoredExceptionHandler(1, WriteWatchHandler);
    DWORD old;
    VirtualProtect(g_ww.page, 0x1000, PAGE_READONLY, &old);
    LOGI("écritures surveillées : %s (%08X)", expr.c_str(), addr);
    HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(proc, nullptr, TRUE);
    long done = 0;
    std::map<std::string, int> seen;
    for (;;) {
        long n = std::min<long>(long(g_ww.count), 256L);
        for (; done < n; done++) {
            const WriteWatch::Hit& h = g_ww.hits[done];
            std::string chain;
            for (uint32_t k = 0; k < h.n; k++) chain += (k ? " <- " : "") + Symbol(proc, h.frames[k]);
            if (seen[chain]++) continue;
            LOGI("écriture de %08X (valeur %08X) : %s", addr, kernel::R32(addr), chain.c_str());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

}  // namespace

void StartProfiler() {
    if (const char* w = std::getenv("XBE_WRITEWATCH")) std::thread(RunWriteWatch, std::string(w)).detach();
    const char* e = std::getenv("XBE_PROFILE");
    if (!e) return;
    int start = std::atoi(e), seconds = 5;
    if (const char* c = std::strchr(e, ',')) seconds = std::atoi(c + 1);
    std::thread(Run, start, seconds).detach();
}

}  // namespace game
