// Diagnostic de blocage : XBE_HANG=<secondes> journalise, au bout de ce délai, les registres et la pile invitée
// de chaque thread du jeu (ceux qui attendent dans le noyau ont un contexte stable).
#include <chrono>
#include <cstdlib>
#include <thread>

#include "core/log.h"
#include "kernel/kernel.h"

namespace game {

void StartHangWatch() {
    const char* v = std::getenv("XBE_HANG");
    if (!v) return;
    int seconds = std::atoi(v);
    std::thread([seconds] {
        std::this_thread::sleep_for(std::chrono::seconds(seconds));
        for (const kernel::ThreadRef& t : kernel::Threads()) {
            if (!t->ctx) continue;
            const X86Context& c = *t->ctx;
            LOGE("hang: thread %X eax %08X ecx %08X edx %08X ebx %08X esp %08X ebp %08X esi %08X edi %08X", t->id, c.eax,
                 c.ecx, c.edx, c.ebx, c.esp, c.ebp, c.esi, c.edi);
            kernel::LogGuestStack(*t->ctx, 96);
        }
        // XBE_HANG_DUMP=adresse[,adresse...] : 16 mots de mémoire invitée à chaque adresse
        if (const char* d = std::getenv("XBE_HANG_DUMP")) {
            for (char* p = const_cast<char*>(d); *p;) {
                uint32_t a = uint32_t(std::strtoul(p, &p, 16));
                for (int i = 0; i < 16; i += 4)
                    LOGE("dump %08X: %08X %08X %08X %08X", a + i * 4, kernel::R32(a + i * 4), kernel::R32(a + i * 4 + 4),
                         kernel::R32(a + i * 4 + 8), kernel::R32(a + i * 4 + 12));
                if (*p == ',') p++;
            }
        }
        logx::Flush();
    }).detach();
}

}  // namespace game
