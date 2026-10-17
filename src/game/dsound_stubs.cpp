// Méthodes CDirectSoundStream que le moteur XACT appelle et que le runtime n'implémente pas : leur code d'origine
// écrit dans les registres de voix de l'APU (structures que les voix du runtime n'ont pas). Réglages de voix
// (3D, cônes, distances) sans équivalent dans le mixeur logiciel : elles réussissent sans effet.
#include "kernel/kernel.h"
#include "xdk/hle.h"

#define CONKER_DSOUND_NOOP(addr, args)    \
    X86_FUNC(sub_##addr) {                \
        (void)base;                       \
        ::kernel::Return(ctx, 0, (args)); \
    }

CONKER_DSOUND_NOOP(004C4B7D, 2)  // (this, valeur) -> voix 0x4C3D..., wrapper 0x4C519C
CONKER_DSOUND_NOOP(004C4BCB, 2)  // wrapper 0x4C51B8
CONKER_DSOUND_NOOP(004C4EA3, 2)  // wrapper 0x4C5230 (.XTLID 0xB0081), écrit via 0x4C3DEB
CONKER_DSOUND_NOOP(004C4EF5, 2)  // wrapper 0x4C5235, écrit via 0x4C3DFE
CONKER_DSOUND_NOOP(004C5D4C, 2)  // jmp depuis 0x4C606D ; XACT 0x54F0AC (descripteur statique 0x5559E8, réglage) :
                                 // passe par l'objet DirectSound global, que le DirectSoundCreate du runtime ne crée pas
