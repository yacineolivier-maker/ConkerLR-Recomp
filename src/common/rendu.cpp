// Rendu modernisé, en extension du moteur D3D12 : occlusion ambiante GTAO multipliée dans la scène 3D dès qu'elle est
// finie (avant le flou lumineux du jeu et l'interface), et SMAA sur l'image présentée. Réglages de settings.toml :
// smaa, occlusion_ambiante, occlusion_rayon (unités du monde), occlusion_force (%). F7 bascule SMAA, F8 l'occlusion.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>

#include "core/log.h"
#include "gpu/ext.h"
#include "rendu_shaders.h"
#include "smaa_shader.h"
#include "smaa_textures.h"

namespace game {

bool g_smaa = true;
bool g_occlusion = true;
uint32_t g_occlusion_rayon = 40;   // unités du monde (le plan proche de la caméra est à 10)
uint32_t g_occlusion_force = 100;  // %

namespace {

using Microsoft::WRL::ComPtr;

constexpr DXGI_FORMAT kAoFormat = DXGI_FORMAT_R16G16_FLOAT;
constexpr DXGI_FORMAT kEdgeFormat = DXGI_FORMAT_R8G8_UNORM;
constexpr DXGI_FORMAT kWeightFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr DXGI_FORMAT kDepthView = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
constexpr uint32_t kMinSceneDraws = 40;  // dessins testés en profondeur avant qu'une cible passe pour la scène
constexpr float kAoMaxSpan = 0.12f;      // de la hauteur de la vue, le plus grand écart des échantillons d'un pixel
constexpr float kAoFadeStart = 2500;     // distance où l'occlusion commence à s'effacer
constexpr float kAoFadeEnd = 8000;

// une cible à nous : ressource, état, vue de rendu dans notre tas
struct Rt {
    ComPtr<ID3D12Resource> res;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    uint32_t w = 0, h = 0;
};

enum Slot { kAo0, kAo1, kAoOut, kEdges, kWeights, kSmaaOut, kSlots };

// projection de la scène, prise des constantes d'un dessin de la scène
struct Camera {
    bool ok = false;
    float proj[4] = {};  // 1 / échelle x y, centre x y
    float zp[2] = {};    // distance = zp[0] / (profondeur - zp[1]), profondeur dans [0, 1] de la vue
    gpu::Viewport vp;
};

// les programmes des pushbuffers de Rare : c3 c4 c5 c6 (registres 99 à 102) = projection, la caméra regardant
// vers -z : x' = c3.x x + c3.z z, w = -z, profondeur = (A z + B) / -z avec c5 = (0, 0, A, B)
bool CameraFromProgram(const gpu::State& s, Camera& cam) {
    const auto& c3 = s.vsc[99];
    const auto& c4 = s.vsc[100];
    const auto& c5 = s.vsc[101];
    const auto& c6 = s.vsc[102];
    bool w_is_minus_z = c6[0] == 0 && c6[1] == 0 && c6[2] == -1 && c6[3] == 0;
    bool depth_row = c5[0] == 0 && c5[1] == 0 && c5[2] < 0 && c5[3] < 0;
    if (!w_is_minus_z || !depth_row || c3[0] <= 0 || c4[1] == 0 || c3[1] != 0 || c4[0] != 0) return false;
    // ndc x = (c3.x x + c3.z z) / -z : x = (ndc + c3.z) d / c3.x avec d = -z
    cam.proj[0] = 1 / c3[0];
    cam.proj[1] = 1 / c4[1];
    cam.proj[2] = -c3[2];
    cam.proj[3] = -c4[2];
    // profondeur = -A - B / z = -A + B / d : d = B / (profondeur + A)
    cam.zp[0] = c5[3];
    cam.zp[1] = -c5[2];
    return true;
}

// D3DTS_PROJECTION (vecteurs lignes, regard vers +z) : x' = m0 x + m8 z, w = z, profondeur = m10 + m14 / z
bool CameraFromTransform(const gpu::State& s, Camera& cam) {
    const auto& m = s.transforms[3];
    if (m[11] != 1 || m[15] != 0 || m[0] <= 0 || m[5] == 0 || m[14] == 0) return false;
    cam.proj[0] = 1 / m[0];
    cam.proj[1] = 1 / m[5];
    cam.proj[2] = m[8];
    cam.proj[3] = m[9];
    cam.zp[0] = m[14];
    cam.zp[1] = m[10];
    return true;
}

class Moderne final : public gpu::Extension {
public:
    void Reset(gpu::Context& c) override {
        for (auto& r : rt_) {
            if (r.res) c.Release(std::move(r.res));
            r = Rt{};
        }
        NewFrame(~0ull);
    }

    bool Bound(gpu::Context& c, const gpu::State& s, gpu::Target* rt, gpu::Target* ds, bool pretransformed) override {
        NewFrame(c.Presents());
        if (!g_occlusion || ao_failed_ || applied_ || !rt) return false;
        bool zen = s.rs[gpu::kRsZEnable] != 0;
        bool scene_draw = ds && zen && !pretransformed && rt->gw >= 320 && !rt->depth;
        if (scene_draw && rt != scene_ && draws_ < kMinSceneDraws) {
            scene_ = rt;  // pas encore assez de dessins : une autre cible en profondeur prend la place
            scene_ds_ = ds;
            draws_ = 0;
        }
        if (scene_draw && rt == scene_ && ds == scene_ds_) {
            draws_++;
            Camera cam;
            if ((s.vs_handle & 1) ? CameraFromProgram(s, cam) : CameraFromTransform(s, cam)) {
                cam.ok = true;
                cam.vp = s.viewport;
                cam_ = cam;
            }
            return false;
        }
        // la scène est finie quand on dessine ailleurs ou sans test de profondeur
        if (scene_ && draws_ >= kMinSceneDraws && cam_.ok) {
            Occlusion(c);
            return true;
        }
        return false;
    }

    void Present(gpu::Context& c, gpu::Target& t) override {
        if (g_occlusion && !ao_failed_ && !applied_ && scene_ == &t && draws_ >= kMinSceneDraws && cam_.ok) Occlusion(c);
        if (g_smaa && !smaa_failed_ && !t.depth) Smaa(c, t);
        NewFrame(~0ull);
    }

private:
    void NewFrame(uint64_t f) {
        if (f == frame_) return;
        frame_ = f;
        applied_ = false;
        scene_ = scene_ds_ = nullptr;
        draws_ = 0;
        cam_ = Camera{};
    }

    D3D12_CPU_DESCRIPTOR_HANDLE View(int i) const {
        D3D12_CPU_DESCRIPTOR_HANDLE h = heap_->GetCPUDescriptorHandleForHeapStart();
        h.ptr += size_t(i) * inc_;
        return h;
    }

    bool Ensure(gpu::Context& c, int i, DXGI_FORMAT fmt, uint32_t w, uint32_t h) {
        Rt& r = rt_[i];
        if (r.res && r.fmt == fmt && r.w == w && r.h == h) return true;
        ID3D12Device* dev = c.Device();
        if (!heap_) {
            D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kSlots, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
            if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap_)))) return false;
            inc_ = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        }
        if (r.res) c.Release(std::move(r.res));
        r = Rt{};
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = w;
        td.Height = h;
        td.DepthOrArraySize = 1;
        td.MipLevels = 1;
        td.Format = fmt;
        td.SampleDesc.Count = 1;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, r.state, nullptr, IID_PPV_ARGS(&r.res))))
            return false;
        dev->CreateRenderTargetView(r.res.Get(), nullptr, View(i));
        r.fmt = fmt;
        r.w = w;
        r.h = h;
        return true;
    }

    void To(gpu::Context& c, int i, D3D12_RESOURCE_STATES want) { c.Transition(rt_[i].res.Get(), rt_[i].state, want); }

    // dst = src (même taille, même format), dst laissé à copier vers lui ; l'état des cibles du moteur est suivi par
    // leur champ state, le moteur les remet en cible de rendu au prochain dessin
    void CopyBack(gpu::Context& c, gpu::Target& dst, int src) {
        To(c, src, D3D12_RESOURCE_STATE_COPY_SOURCE);
        c.Transition(dst.res.Get(), dst.state, D3D12_RESOURCE_STATE_COPY_DEST);
        c.List()->CopyResource(dst.res.Get(), rt_[src].res.Get());
    }

    // la profondeur de la scène en occlusion, floutée en deux passes, multipliée dans sa couleur
    void Occlusion(gpu::Context& c) {
        applied_ = true;
        gpu::Target& rt = *scene_;
        gpu::Target& ds = *scene_ds_;
        uint32_t w = rt.w, h = rt.h;
        if (!Ensure(c, kAo0, kAoFormat, w, h) || !Ensure(c, kAo1, kAoFormat, w, h) || !Ensure(c, kAoOut, rt.fmt, w, h)) {
            ao_failed_ = true;
            return;
        }
        c.Unbind();
        // la vue du jeu en pixels de la cible
        float sc = float(rt.w) / float(rt.gw);
        float vx = std::clamp(cam_.vp.x * sc, 0.0f, float(w)), vy = std::clamp(cam_.vp.y * sc, 0.0f, float(h));
        float vw = std::clamp(cam_.vp.w * sc, 1.0f, float(w) - vx), vh = std::clamp(cam_.vp.h * sc, 1.0f, float(h) - vy);
        // la profondeur va de min_z à max_z sur la vue : ramenée à [0, 1] pour la projection
        float zr = cam_.vp.max_z - cam_.vp.min_z;
        float p[20] = {cam_.proj[0], cam_.proj[1], cam_.proj[2], cam_.proj[3],
                       cam_.zp[0] * zr, cam_.vp.min_z + cam_.zp[1] * zr, 0, 0,
                       vx, vy, vw, vh,
                       float(g_occlusion_rayon), kAoMaxSpan * vh, 0, 0,
                       kAoFadeStart, kAoFadeEnd, std::min(g_occlusion_force, 200u) / 100.0f, 0};
        c.Transition(ds.res.Get(), ds.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        const DXGI_FORMAT f[3] = {kDepthView, kAoFormat, rt.fmt};
        ID3D12Resource* const raw[3] = {ds.res.Get(), nullptr, nullptr};
        To(c, kAo0, D3D12_RESOURCE_STATE_RENDER_TARGET);
        bool ok = c.Pass(kAoHlsl, "gtao", kAoFormat, View(kAo0), w, h, raw, f, p, -1, 20);
        To(c, kAo0, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        To(c, kAo1, D3D12_RESOURCE_STATE_RENDER_TARGET);
        p[14] = 1;
        ID3D12Resource* const across[3] = {ds.res.Get(), rt_[kAo0].res.Get(), nullptr};
        ok = ok && c.Pass(kAoHlsl, "blur", kAoFormat, View(kAo1), w, h, across, f, p, -1, 20);
        To(c, kAo1, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        To(c, kAoOut, D3D12_RESOURCE_STATE_RENDER_TARGET);
        c.Transition(rt.res.Get(), rt.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        p[14] = 0;
        p[15] = 1;
        ID3D12Resource* const down[3] = {ds.res.Get(), rt_[kAo1].res.Get(), rt.res.Get()};
        ok = ok && c.Pass(kAoHlsl, "apply", rt.fmt, View(kAoOut), w, h, down, f, p, -1, 20);
        if (!ok) {
            LOGI("rendu : occlusion ambiante désactivée, un de ses shaders n'a pas pu être fait");
            ao_failed_ = true;
            return;
        }
        CopyBack(c, rt, kAoOut);
        LOG_FIRST(1, "rendu : occlusion ambiante GTAO %ux%u, projection %.4f %.4f, distance %.4f / (profondeur - %.6f)", w, h,
                  1 / cam_.proj[0], 1 / cam_.proj[1], cam_.zp[0], cam_.zp[1]);
    }

    // les tables de SMAA (smaa_textures.h : aire 160 x 560 sur deux canaux, recherche 64 x 16 sur un), une fois
    bool Lookups(gpu::Context& c) {
        if (area_ && search_) return true;
        struct Table {
            ComPtr<ID3D12Resource>* res;
            DXGI_FORMAT fmt;
            uint32_t w, h, bpp;
            const uint8_t* data;
        };
        static_assert(sizeof(gpu::kSmaaArea) == 160 * 560 * 2 && sizeof(gpu::kSmaaSearch) == 64 * 16);
        const Table tables[2] = {{&area_, DXGI_FORMAT_R8G8_UNORM, 160, 560, 2, gpu::kSmaaArea},
                                 {&search_, DXGI_FORMAT_R8_UNORM, 64, 16, 1, gpu::kSmaaSearch}};
        ID3D12Device* dev = c.Device();
        constexpr uint32_t kPitch = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
        uint64_t offsets[2] = {0, 0};
        uint64_t total = 0;
        for (int i = 0; i < 2; i++) {
            offsets[i] = total;
            uint32_t pitch = (tables[i].w * tables[i].bpp + kPitch - 1) / kPitch * kPitch;
            total += (uint64_t(pitch) * tables[i].h + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) /
                     D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT * D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
        }
        D3D12_HEAP_PROPERTIES up{D3D12_HEAP_TYPE_UPLOAD};
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = total;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                IID_PPV_ARGS(&upload_))))
            return false;
        uint8_t* cpu = nullptr;
        if (FAILED(upload_->Map(0, nullptr, reinterpret_cast<void**>(&cpu)))) return false;
        for (int i = 0; i < 2; i++) {
            const Table& t = tables[i];
            uint32_t row = t.w * t.bpp, pitch = (row + kPitch - 1) / kPitch * kPitch;
            for (uint32_t y = 0; y < t.h; y++) std::memcpy(cpu + offsets[i] + size_t(y) * pitch, t.data + size_t(y) * row, row);
            D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
            D3D12_RESOURCE_DESC td{};
            td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            td.Width = t.w;
            td.Height = t.h;
            td.DepthOrArraySize = 1;
            td.MipLevels = 1;
            td.Format = t.fmt;
            td.SampleDesc.Count = 1;
            if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                    IID_PPV_ARGS(t.res->GetAddressOf()))))
                return false;
            D3D12_TEXTURE_COPY_LOCATION dl{t.res->Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
            dl.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION sl{upload_.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
            sl.PlacedFootprint.Offset = offsets[i];
            sl.PlacedFootprint.Footprint = {t.fmt, t.w, t.h, 1, pitch};
            c.List()->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
            D3D12_RESOURCE_STATES st = D3D12_RESOURCE_STATE_COPY_DEST;
            c.Transition(t.res->Get(), st, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
        upload_->Unmap(0, nullptr);
        return true;
    }

    // bords, poids de mélange, puis l'image mélangée recopiée dans la cible présentée
    void Smaa(gpu::Context& c, gpu::Target& t) {
        uint32_t w = t.w, h = t.h;
        if (!Lookups(c) || !Ensure(c, kEdges, kEdgeFormat, w, h) || !Ensure(c, kWeights, kWeightFormat, w, h) ||
            !Ensure(c, kSmaaOut, t.fmt, w, h)) {
            smaa_failed_ = true;
            LOGI("rendu : SMAA désactivé, ses cibles n'ont pas pu être faites");
            return;
        }
        if (smaa_src_[0].empty()) {
            std::string base = std::string(kSmaaConfig) + gpu::kSmaaHlsl + kSmaaPasses;
            smaa_src_[0] = base + kSmaaEdgesVs;
            smaa_src_[1] = base + kSmaaWeightsVs;
            smaa_src_[2] = base + kSmaaBlendVs;
        }
        c.Unbind();
        float p[8] = {1.0f / w, 1.0f / h, float(w), float(h)};
        const float clear[4] = {};
        c.Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        // les bords écartent les pixels sans bord : leur cible part vide à chaque image
        To(c, kEdges, D3D12_RESOURCE_STATE_RENDER_TARGET);
        c.List()->ClearRenderTargetView(View(kEdges), clear, 0, nullptr);
        ID3D12Resource* const edges[3] = {t.res.Get(), nullptr, nullptr};
        const DXGI_FORMAT edges_fmt[3] = {t.fmt, t.fmt, t.fmt};
        bool ok = c.Pass(smaa_src_[0].c_str(), "smaa_edges", kEdgeFormat, View(kEdges), w, h, edges, edges_fmt, p, -1);
        To(c, kEdges, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        To(c, kWeights, D3D12_RESOURCE_STATE_RENDER_TARGET);
        c.List()->ClearRenderTargetView(View(kWeights), clear, 0, nullptr);
        ID3D12Resource* const weights[3] = {rt_[kEdges].res.Get(), area_.Get(), search_.Get()};
        const DXGI_FORMAT weights_fmt[3] = {kEdgeFormat, DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R8_UNORM};
        ok = ok && c.Pass(smaa_src_[1].c_str(), "smaa_weights", kWeightFormat, View(kWeights), w, h, weights, weights_fmt, p, -1);
        To(c, kWeights, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        To(c, kSmaaOut, D3D12_RESOURCE_STATE_RENDER_TARGET);
        ID3D12Resource* const blend[3] = {t.res.Get(), rt_[kWeights].res.Get(), nullptr};
        const DXGI_FORMAT blend_fmt[3] = {t.fmt, kWeightFormat, t.fmt};
        ok = ok && c.Pass(smaa_src_[2].c_str(), "smaa_blend", t.fmt, View(kSmaaOut), w, h, blend, blend_fmt, p, -1);
        if (!ok) {
            LOGI("rendu : SMAA désactivé, un de ses shaders n'a pas pu être fait");
            smaa_failed_ = true;
            return;
        }
        CopyBack(c, t, kSmaaOut);
        LOG_FIRST(1, "rendu : SMAA %ux%u", w, h);
    }

    Rt rt_[kSlots];
    ComPtr<ID3D12DescriptorHeap> heap_;
    uint32_t inc_ = 0;
    ComPtr<ID3D12Resource> area_, search_, upload_;
    std::string smaa_src_[3];
    bool ao_failed_ = false, smaa_failed_ = false;
    // l'image en cours
    uint64_t frame_ = ~0ull;
    bool applied_ = false;
    gpu::Target* scene_ = nullptr;
    gpu::Target* scene_ds_ = nullptr;
    uint32_t draws_ = 0;
    Camera cam_;
};

}  // namespace

void InstallRendu() { gpu::SetExtension(std::make_unique<Moderne>()); }

// F7 : SMAA, F8 : occlusion ambiante, le temps d'une comparaison (settings.toml garde le choix permanent)
bool RenduKey(uint32_t vk) {
    if (vk == VK_F7) {
        g_smaa = !g_smaa;
        LOGI("rendu : SMAA %s", g_smaa ? "activé" : "désactivé");
        return true;
    }
    if (vk == VK_F8) {
        g_occlusion = !g_occlusion;
        LOGI("rendu : occlusion ambiante %s", g_occlusion ? "activée" : "désactivée");
        return true;
    }
    return false;
}

}  // namespace game
