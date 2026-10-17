// Vidéos XMV : le décodeur écrit chaque image directement dans la mémoire de la texture affichée, sans Lock ni
// rien que le moteur de rendu voie ; la texture est marquée modifiée à chaque dessin du quad vidéo, sans quoi la
// première image reçue resterait affichée (ou une image noire).
#include "core/log.h"
#include "gpu/texture.h"
#include "kernel/kernel.h"
#include "xdk/hle.h"

// avant le BeginPush du quad vidéo (0x252F9A) : texture [0x849C50 + index * 4], index en 0x779B98
void MarkVideoFrame(X86Context& ctx, uint8_t* base) {
    (void)ctx;
    (void)base;
    uint32_t texture = kernel::R32(0x849C50 + 4 * kernel::R32(0x779B98));
    gpu::MarkDirty(texture);
    static int n = 0;
    if (n++ % 60 == 0) {
        uint32_t dev = kernel::R32(0x5499E8);
        LOGI("video: texture %08X data %08X, étage 0 du device %08X, flags sales %08X, push %08X", texture,
             kernel::R32(texture + 4), kernel::R32(dev + 0xF88), kernel::R32(0x5499F0), kernel::R32(dev));
    }
}
