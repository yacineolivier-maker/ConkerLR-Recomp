// Profileur par échantillonnage (diagnostic) : XBE_PROFILE=<début s>,<durée s> relève toutes les millisecondes
// l'adresse d'exécution de chaque thread du processus, puis journalise les fonctions les plus vues par thread
// (noms des symboles publics du PDB : les fonctions recompilées s'appellent sub_<adresse invitée>).
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
    auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < end) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 te{sizeof(te)};
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
            HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
            if (!t) continue;
            if (SuspendThread(t) != DWORD(-1)) {
                CONTEXT c{};
                c.ContextFlags = CONTEXT_CONTROL;
                if (GetThreadContext(t, &c)) {
                    hits[te.th32ThreadID][c.Rip]++;
                    totals[te.th32ThreadID]++;
                }
                ResumeThread(t);
            }
            CloseHandle(t);
        }
        CloseHandle(snap);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
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
        LOGI("profil : thread %lu, %u échantillons", tid, total);
        for (size_t i = 0; i < v.size() && i < 25; i++)
            LOGI("profil :   %5.1f %%  %s", 100.0 * v[i].first / total, v[i].second.c_str());
    }
    logx::Flush();
}

}  // namespace

void StartProfiler() {
    const char* e = std::getenv("XBE_PROFILE");
    if (!e) return;
    int start = std::atoi(e), seconds = 5;
    if (const char* c = std::strchr(e, ',')) seconds = std::atoi(c + 1);
    std::thread(Run, start, seconds).detach();
}

}  // namespace game
