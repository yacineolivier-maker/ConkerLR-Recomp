// Direct3D 8 (XDK 5849) entry points the title calls, implemented on the host renderer.
// Resource creation, locking and the XG helpers stay recompiled: they only manage memory.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "app/app.h"
#include "core/diag.h"
#include "gpu/backend.h"
#include "gpu/texture.h"
#include "gpu/vertex.h"
#include "xdk/d3d8.h"
#include "xdk/hle.h"
#include "xdk/layout.h"

using namespace kernel;

// the push buffer executor's method handling (below), which pushes the title writes itself go through as well
namespace {
namespace pb {
void Method(uint32_t m, uint32_t v);
// the vertex program loaded at an address of the GPU's program memory, up to its last instruction
std::vector<uint32_t> ProgramFrom(uint32_t start);
// a program put in the GPU's program memory at address, as SetVertexShader loads its shader's at 0
void LoadProgram(const std::vector<uint32_t>& prog, uint32_t address);
uint64_t Fnv(const void* p, size_t n, uint64_t h);
// the bound program read with the packed vertices of an inline array, laid out by the pushed array formats
std::shared_ptr<gpu::VertexShader> InlineLayout(const std::shared_ptr<gpu::VertexShader>& program, uint32_t& stride);
extern bool g_fog_seen;
}  // namespace pb
}  // namespace

// configs written before the SDK knew the name
#ifndef SYM_D3DDevice_BeginPush
#define SYM_D3DDevice_BeginPush FFFFFE00
#define HAS_D3DDevice_BeginPush 0
#endif
#ifndef SYM_D3DDevice_EndPush
#define SYM_D3DDevice_EndPush FFFFFE01
#define HAS_D3DDevice_EndPush 0
#endif
#ifndef HAS_D3D_FvfShader
#define HAS_D3D_FvfShader 0
#endif
#ifndef SYM_D3DDevice_SetPalette
#define SYM_D3DDevice_SetPalette FFFFFE02
#define HAS_D3DDevice_SetPalette 0
#endif
#ifndef SYM_D3DDevice_GetVertexShaderFunction
#define SYM_D3DDevice_GetVertexShaderFunction FFFFFE03
#define HAS_D3DDevice_GetVertexShaderFunction 0
#endif
#ifndef SYM_D3DDevice_RunPushBuffer
#define SYM_D3DDevice_RunPushBuffer FFFFFE05
#define HAS_D3DDevice_RunPushBuffer 0
#endif
#ifndef SYM_D3DDevice_Reset
#define SYM_D3DDevice_Reset FFFFFE04
#define HAS_D3DDevice_Reset 0
#endif

namespace d3d {

namespace {

// A/B switch: 0 draws on the title's thread, as before
#ifndef XBE_RENDER_THREAD
#define XBE_RENDER_THREAD 1
#endif
constexpr bool kRenderThread = XBE_RENDER_THREAD;

// D3D globals of this build
constexpr uint32_t kDevice = SYM(D3D_Device);        // the static CDevice, g_pDevice points at it
constexpr uint32_t kDevicePtr = SYM(D3D_pDevice);
constexpr uint32_t kIndexData = SYM(D3D_IndexData);  // index data base the inline draws read
constexpr uint32_t kDirtyFlags = SYM(D3D_DirtyFlags);
constexpr uint32_t kTextureState = SYM(D3D_DeferredTextureState);  // [4][32]
constexpr uint32_t kRenderState = SYM(D3D_RenderState);            // [166]
constexpr uint32_t kStreams = SYM(D3D_Streams);                    // [16] stride, offset, buffer
constexpr uint32_t kSimpleEncode = SYM(D3D_SimpleRenderStateEncode);
// (device, vertex format key): sends the pending deferred states (LazySetStates) and the current shader's vertex
// formats unless the key matches the one sent last, then keeps the key. RunPushBuffer calls it with 0 before the
// buffer, so the draws after it send their formats again. Not in the config: found by hand, called at 0x53AE41
constexpr uint32_t kSetStateFlags = 0x543200;
// XBE_WATCH_ADDR=<physical hex>: logs the texture binds, push buffer texture offsets and render targets naming it
bool Watched(uint32_t a) {
    static const uint32_t w = std::getenv("XBE_WATCH_ADDR") ? uint32_t(std::strtoul(std::getenv("XBE_WATCH_ADDR"), nullptr, 16)) : 0;
    return w && (a & 0x0FFFFFFF) == (w & 0x0FFFFFFF);
}

// CDevice fields mirrored so the recompiled getters and resource code see what the XDK would
constexpr uint32_t kDevPush = XDK_CDevice_Push, kDevThreshold = XDK_CDevice_PushThreshold,
                   kDevBaseVertex = XDK_CDevice_BaseVertex, kDevIndexBuffer = XDK_CDevice_IndexBuffer,
                   kDevTextures = XDK_CDevice_Textures, kDevRenderTarget = XDK_CDevice_RenderTarget,
                   kDevDepth = XDK_CDevice_Depth, kDevSwapCount = XDK_CDevice_SwapCount,
                   kDevBackBuffer = XDK_CDevice_BackBuffer, kDevFrontBuffer = XDK_CDevice_FrontBuffer,
                   kDevZScale = XDK_CDevice_ZScale, kDevSuperX = XDK_CDevice_SuperSampleX,
                   kDevSuperY = XDK_CDevice_SuperSampleY, kDevTransforms = XDK_CDevice_Transforms,
                   kDevPalettes = XDK_CDevice_Palettes;
constexpr uint32_t kStageStates = XDK_TextureState_Stage, kStreamSize = XDK_Stream_Size;

gpu::State g_state;
std::unique_ptr<gpu::Backend> g_backend;
std::recursive_mutex g_lock;

std::unordered_map<uint32_t, std::shared_ptr<gpu::VertexShader>> g_vs;
std::unordered_map<uint32_t, std::shared_ptr<gpu::PixelShader>> g_ps;
std::array<uint32_t, 3> g_buffers{};  // front, back surfaces
uint32_t g_buffer_count = 2;
uint32_t g_depth = 0;
uint32_t g_scratch_push = 0, g_scratch_size = 0;
uint32_t g_walk = 0;  // first scratch command not read yet

uint32_t Dev(uint32_t off) { return R32(kDevice + off); }
void SetDev(uint32_t off, uint32_t v) { W32(kDevice + off, v); }

// The device's own references: a count in bits 19 to 22 of the resource's first dword. A surface's first one
// also holds its parent. The library's functions do it when the image has them, inlined ones are done here.
void AddRef(X86Context& ctx, uint32_t res) {
    if (!res) return;
#if HAS_D3D_InternalAddRef
    GUEST_CALL(D3D_InternalAddRef, ctx, {res});
#else
    (void)ctx;
    uint32_t common = R32(res);
    if (!(common & 0x780000) && (common & 0x70000) == 0x50000)
        if (uint32_t parent = R32(res + 0x14)) W32(parent, R32(parent) + 0x80000);
    W32(res, R32(res) + 0x80000);
#endif
}
void Release(X86Context& ctx, uint32_t res) {
    if (!res) return;
#if HAS_D3D_InternalRelease
    GUEST_CALL(D3D_InternalRelease, ctx, {res});
#else
    (void)ctx;
    // the library frees a resource nothing holds any more; without its function the memory stays
    auto drop = [](uint32_t r) {
        W32(r, R32(r) - 0x80000);
        if (!(R32(r) & 0x78FFFF)) LOG_FIRST(8, "d3d: resource %08X is not freed, the image has no D3D_InternalRelease", r);
    };
    uint32_t common = R32(res);
    if ((common & 0x780000) == 0x80000 && (common & 0x70000) == 0x50000)
        if (uint32_t parent = R32(res + 0x14)) drop(parent);
    drop(res);
#endif
}

void Walk(uint32_t end);

// Hardware commands a caller still pushes go to a scratch buffer, rewound at every request once Walk has read it
uint32_t ScratchPush(uint32_t bytes = 0) {
    Walk(Dev(kDevPush));
    uint32_t need = std::max<uint32_t>(0x10000, (bytes + 0x1FFF) & ~0xFFFu);
    if (need > g_scratch_size) {
        if (g_scratch_push) mem::FreeContiguous(g_scratch_push);
        g_scratch_push = mem::AllocContiguous(need, 0, 0xFFFFFFFF, 0x1000);
        g_scratch_size = need;
    }
    SetDev(kDevPush, g_scratch_push);
    SetDev(kDevThreshold, g_scratch_push + g_scratch_size - 0x1000);
    g_walk = g_scratch_push;
    return g_scratch_push;
}

gpu::Surface SurfaceOf(uint32_t header) {
    gpu::Surface s;
    s.header = header;
    if (header) {
        s.data = R32(header + 4);
        s.format = R32(header + 12);
        s.size = R32(header + 16);
    }
    return s;
}

uint32_t NewSurface(uint32_t w, uint32_t h, uint32_t fmt, uint32_t bpp) {
    uint32_t pitch = (w * bpp + 63) & ~63u;
    uint32_t data = mem::AllocContiguous(pitch * h, 0, 0xFFFFFFFF, 0x1000);
    uint32_t hdr = mem::AllocSystem(24);
    W32(hdr + 0, 0x000D0001);  // surface, one reference, one internal reference held by the device
    W32(hdr + 4, mem::PhysicalFromVirtual(data));
    W32(hdr + 8, 0);
    W32(hdr + 12, 0x00010021 | (fmt << 8));  // dma A, 2D, one level
    W32(hdr + 16, (w - 1) | (h - 1) << 12 | (pitch / 64 - 1) << 24);
    W32(hdr + 20, 0);
    return hdr;
}

void SyncDeferred() {
    // deferred states live in the title's memory, the inline SetRenderState and SetTextureStageState write them
    for (uint32_t i = gpu::kRsDeferredFirst; i < gpu::kRsComplexFirst; i++) g_state.rs[i] = R32(kRenderState + i * 4);
    // with the bump matrices and coordinate index: not deferred, but the library's setters (inlined by this title)
    // store them in the same table before pushing them
    for (int s = 0; s < 4; s++)
        for (uint32_t i = 0; i <= gpu::kTssTexCoordIndex; i++) g_state.tss[s][i] = R32(kTextureState + (s * kStageStates + i) * 4);
    // the border colour is no deferred state: the inline SetTextureStageState pushes it (SET_TEXTURE_BORDER_COLOR)
    for (uint32_t s = 0; s < 4; s++) g_state.tss[s][gpu::kTssBorderColor] = g_state.simple[(0x1B24 + s * 0x40) >> 2];
    // complex states the title's inlined setters write to the library's table (z bias, YUV, edge antialiasing, the
    // table's own defaults such as texture factor FFFFFFFF): taken when the table changed since last read, so a push
    // buffer's cull, depth and stencil registers still hold until the title sets them again
    static std::array<uint32_t, gpu::kRsMax> seen = [] {
        std::array<uint32_t, gpu::kRsMax> a;
        a.fill(0xDEADBEEF);
        return a;
    }();
    for (uint32_t i = gpu::kRsComplexFirst; i < gpu::kRsMax; i++) {
        uint32_t v = R32(kRenderState + i * 4);
        if (v != seen[i]) g_state.rs[i] = seen[i] = v;
    }
}

float ZScale(const gpu::Surface& zs) {
    uint32_t f = zs.header ? (zs.format >> 8) & 0xFF : 0x2E;
    return f == 0x2C || f == 0x30 ? 65535.0f : 16777215.0f;
}

bool g_own_constants = false;  // D3DSCM_NORESERVEDCONSTANTS: c-38 and c-37 are the title's

// c-38 and c-37 hold the viewport scale and offset, as D3D_GetViewportOffsetAndScale computes them
void UpdateViewportConstants() {
    auto& v = g_state.viewport;
    g_state.zscale = ZScale(g_state.zs);
    float hw = v.w * 0.5f, hh = v.h * 0.5f;
    if (!g_own_constants) {
        g_state.vsc[58] = {hw, -hh, (v.max_z - v.min_z) * g_state.zscale, 0};
        g_state.vsc[59] = {v.x + g_state.ss_offset_x + hw, v.y + g_state.ss_offset_y + hh, v.min_z * g_state.zscale, 0};
    }
    // the viewport as the library's SetViewport stores it, for inlined readers
    if (XDK_CDevice_Viewport != xdk::kUnknown) {
        uint32_t p = kDevice + XDK_CDevice_Viewport;
        W32(p, v.x);
        W32(p + 4, v.y);
        W32(p + 8, v.w);
        W32(p + 12, v.h);
        std::memcpy(HostPtr(p + 16), &v.min_z, 4);
        std::memcpy(HostPtr(p + 20), &v.max_z, 4);
    }
}

void SetComplex(uint32_t rs, uint32_t v) {
    g_state.rs[rs] = v;
    W32(kRenderState + rs * 4, v);
    // the hardware methods the XDK pushes for them, which push buffers run with
    auto& m = g_state.simple;
    switch (rs) {
        case gpu::kRsZEnable: m[0x30C >> 2] = v != 0; break;
        case gpu::kRsStencilEnable: m[0x32C >> 2] = v != 0; break;
        case gpu::kRsStencilFail: m[0x370 >> 2] = v; break;
        case gpu::kRsFillMode: m[0x38C >> 2] = v; m[0x390 >> 2] = v; break;
        case gpu::kRsCullMode: {  // as the XDK pushes it: cull the front when the mode names the front face's winding
            uint32_t front = g_state.rs[gpu::kRsFrontFace] ? g_state.rs[gpu::kRsFrontFace] : 0x900;
            m[0x308 >> 2] = v != 0;
            if (v) m[0x39C >> 2] = v == front ? 0x404 : 0x405;
            m[0x3A0 >> 2] = front ^ 1;
            break;
        }
        case gpu::kRsFrontFace: m[0x3A0 >> 2] = (v ? v : 0x900) ^ 1; break;
        default: break;
    }
}

// The library's default render and texture stage states; its pushes land in the scratch buffer
void ApplyDefaultStates(X86Context& ctx) {
#if HAS_D3D_InitDefaultState
    GUEST_CALL(D3D_InitDefaultState, ctx, {});
#else
    (void)ctx;
    LOG_FIRST(1, "d3d: the image has no D3D_InitDefaultState, default states were not applied");
#endif
}

constexpr bool kVBlank = XDK_CDevice_VBlankCount != xdk::kUnknown && XDK_CDevice_VBlankEvent != xdk::kUnknown &&
                         XDK_CDevice_VBlankCallback != xdk::kUnknown;

// what the library's vertical blank interrupt leaves for the title: count, event, callback
void VerticalBlank(X86Context& ctx) {
    using clock = std::chrono::steady_clock;
    uint32_t info = mem::AllocSystem(12), shown = 0;
    auto next = clock::now();
    for (;;) {
        next = std::max(next + std::chrono::microseconds(16667), clock::now());
        std::this_thread::sleep_until(next);
        uint32_t count = Dev(XDK_CDevice_VBlankCount) + 1, swaps = Dev(kDevSwapCount);
        SetDev(XDK_CDevice_VBlankCount, count);
        ObjectRef event = FromPointer(kDevice + XDK_CDevice_VBlankEvent);
        SetEvent(*event);
        if (uint32_t callback = Dev(XDK_CDevice_VBlankCallback)) {
            // blank count, swaps shown, 1 when this blank showed one
            W32(info, count);
            W32(info + 4, swaps);
            W32(info + 8, swaps != shown);
            CallGuest(ctx, callback, {info});
        }
        shown = swaps;
        // the title clears the event in its own memory before a wait, which the host object does not see
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ResetEvent(*event);
    }
}

}  // namespace

gpu::State& State() { return g_state; }
std::recursive_mutex& Lock() { return g_lock; }

}  // namespace d3d

using namespace d3d;

// device

HLE_STDCALL(Direct3D_CreateDevice, 6) {
    XDK_REQUIRE("d3d", XDK_DATA(D3D_Device), XDK_DATA(D3D_pDevice), XDK_DATA(D3D_DirtyFlags),
                XDK_DATA(D3D_DeferredTextureState), XDK_DATA(D3D_RenderState), XDK_DATA(D3D_Streams),
                XDK_DATA(D3D_SimpleRenderStateEncode));
    XDK_REQUIRE("d3d", XDK_KEY(CDevice_Push), XDK_KEY(CDevice_PushThreshold), XDK_KEY(CDevice_BaseVertex),
                XDK_KEY(CDevice_IndexBuffer), XDK_KEY(CDevice_ZScale), XDK_KEY(CDevice_SuperSampleX),
                XDK_KEY(CDevice_SuperSampleY), XDK_KEY(CDevice_Transforms), XDK_KEY(CDevice_Textures),
                XDK_KEY(CDevice_RenderTarget), XDK_KEY(CDevice_Depth), XDK_KEY(CDevice_BackBuffer),
                XDK_KEY(CDevice_FrontBuffer), XDK_KEY(CDevice_SwapCount), XDK_KEY(CDevice_Size),
                XDK_KEY(DirtyFlags_Texture), XDK_KEY(TextureState_Stage), XDK_KEY(Stream_Size),
                XDK_KEY(Stream_Stride), XDK_KEY(Stream_Buffer));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t pp = a[4];  // D3DPRESENT_PARAMETERS
    uint32_t w = R32(pp), h = R32(pp + 4), fmt = R32(pp + 8), count = std::max<uint32_t>(R32(pp + 12), 1);
    uint32_t auto_depth = R32(pp + 0x20), depth_fmt = R32(pp + 0x24), interval = R32(pp + 0x30);
    if (!w) w = 640;
    if (!h) h = 480;
    LOGI("d3d: CreateDevice %ux%u format %X buffers %u depth %u format %X interval %X flags %X", w, h, fmt, count,
         auto_depth, depth_fmt, interval, R32(pp + 0x28));
    std::memset(HostPtr(kDevice), 0, XDK_CDevice_Size);
    W32(kDevicePtr, kDevice);
    ScratchPush();

    uint32_t color_fmt = gpu::LinearColorFormat(fmt);
    g_buffer_count = std::min<uint32_t>(count + 1, 3);
    for (uint32_t i = 0; i < g_buffer_count; i++) g_buffers[i] = NewSurface(w, h, color_fmt, 4);
    g_depth = 0;
    if (auto_depth) {
        uint32_t zf = gpu::LinearDepthFormat(depth_fmt);
        g_depth = NewSurface(w, h, zf, zf == 0x30 || zf == 0x31 ? 2 : 4);  // 0x30 LIN_D16, 0x31 LIN_F16
    }
    if (!g_backend) {
        g_backend = gpu::CreateBackend();
        if (!g_backend->Init(w, h)) LOGE("d3d: renderer init failed");
        if (kRenderThread) g_backend = gpu::CreateThreadedBackend(std::move(g_backend));
    }
    g_state = gpu::State{};
    g_state.viewport = {0, 0, w, h, 0.0f, 1.0f};
    SetDev(kDevRenderTarget, g_buffers[1]);
    SetDev(kDevDepth, g_depth);
    SetDev(kDevBackBuffer, g_buffers[1]);
    SetDev(kDevFrontBuffer, g_buffers[1]);
    float one = 1.0f, zs = g_depth && (R32(g_depth + 12) >> 8 & 0xFF) == 0x30 ? 65535.0f : 16777215.0f;
    std::memcpy(HostPtr(kDevice + kDevSuperX), &one, 4);
    std::memcpy(HostPtr(kDevice + kDevSuperY), &one, 4);
    std::memcpy(HostPtr(kDevice + kDevZScale), &zs, 4);
    // fields the library's init sets that inlined library code and the default states read
    if (XDK_CDevice_AutoDepth != xdk::kUnknown) SetDev(XDK_CDevice_AutoDepth, g_depth);
    if (XDK_CDevice_BackBufferScale != xdk::kUnknown) {
        std::memcpy(HostPtr(kDevice + XDK_CDevice_BackBufferScale), &one, 4);
        std::memcpy(HostPtr(kDevice + XDK_CDevice_BackBufferScale + 4), &one, 4);
    }
    // 60 Hz, widescreen
    if (XDK_CDevice_DisplayFlags != xdk::kUnknown) SetDev(XDK_CDevice_DisplayFlags, 0x400000 | (app::Widescreen() ? 0x10000 : 0));
#if HAS_D3D_FvfShader
    if (XDK_CDevice_VertexShader != xdk::kUnknown) SetDev(XDK_CDevice_VertexShader, SYM(D3D_FvfShader));
#endif
    if (kVBlank) {
        // notification event, signalled
        SetDev(XDK_CDevice_VBlankEvent, 0x00040000);
        SetDev(XDK_CDevice_VBlankEvent + 4, 1);
        static bool started = false;
        if (!started) StartHostThread("vertical blank", VerticalBlank);
        started = true;
    }
    g_state.rt = SurfaceOf(g_buffers[1]);
    g_state.zs = SurfaceOf(g_depth);
    UpdateViewportConstants();
    ApplyDefaultStates(ctx);
    if (a[5]) W32(a[5], kDevice);
    return 0;
}

// Reset(present parameters): the swap chain at a new size or format. The library's own Reset reprograms the
// NV2A's FIFO and display registers, which the host has none of: the buffers are made anew, as CreateDevice does.
HLE_STDCALL(D3DDevice_Reset, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));  // what the title pushed itself before this call comes first
    uint32_t pp = a[0];
    uint32_t w = R32(pp), h = R32(pp + 4), fmt = R32(pp + 8), count = std::max<uint32_t>(R32(pp + 12), 1);
    uint32_t auto_depth = R32(pp + 0x20), depth_fmt = R32(pp + 0x24);
    if (!w) w = 640;
    if (!h) h = 480;
    LOGI("d3d: Reset %ux%u format %X buffers %u depth %u format %X flags %X", w, h, fmt, count, auto_depth, depth_fmt,
         R32(pp + 0x28));
    uint32_t color_fmt = gpu::LinearColorFormat(fmt);
    g_buffer_count = std::min<uint32_t>(count + 1, 3);
    for (uint32_t i = 0; i < g_buffer_count; i++) g_buffers[i] = NewSurface(w, h, color_fmt, 4);
    g_depth = 0;
    if (auto_depth) {
        uint32_t zf = gpu::LinearDepthFormat(depth_fmt);
        g_depth = NewSurface(w, h, zf, zf == 0x30 || zf == 0x31 ? 2 : 4);
    }
    SetDev(kDevRenderTarget, g_buffers[1]);
    SetDev(kDevDepth, g_depth);
    SetDev(kDevBackBuffer, g_buffers[1]);
    SetDev(kDevFrontBuffer, g_buffers[1]);
    if (XDK_CDevice_AutoDepth != xdk::kUnknown) SetDev(XDK_CDevice_AutoDepth, g_depth);
    float zs = g_depth && (R32(g_depth + 12) >> 8 & 0xFF) == 0x30 ? 65535.0f : 16777215.0f;
    std::memcpy(HostPtr(kDevice + kDevZScale), &zs, 4);
    g_state.viewport = {0, 0, w, h, 0.0f, 1.0f};
    g_state.rt = SurfaceOf(g_buffers[1]);
    g_state.zs = SurfaceOf(g_depth);
    UpdateViewportConstants();
    return 0;
}

HLE_STDCALL(D3DDevice_Release, 0) { return 1; }

// synchronisation, the renderer never runs behind

HLE_STDCALL(D3D_BlockOnResource, 1) {
    uint32_t res = a[0];
    if (res && g_backend) {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        uint32_t type = R32(res) & 0x70000;  // 0x40000 texture, 0x50000 surface
        if (type == 0x40000 || type == 0x50000) {
            uint32_t data = mem::VirtualFromPhysical(R32(res + 4));
            // perf diagnostic: who blocks on which surface, once each
            static std::unordered_set<uint32_t> seen;
            if (diag::Perf() && seen.insert(data).second) {
                uint32_t code[4] = {}, n = 0;
                for (uint32_t i = 0; i < 32 && n < 4; i++) {
                    uint32_t v = R32(ctx.esp + i * 4);
                    if (v >= X86CodeBase && v < X86CodeEnd) code[n++] = v;
                }
                LOGI("d3d: BlockOnResource on surface %08X, stack %08X %08X %08X %08X", data, code[0], code[1], code[2], code[3]);
            }
            g_backend->Sync(data, 0);
        } else {
            g_backend->WaitResource(res);
        }
    }
    gpu::MarkDirty(res);
    return 0;
}
// lock(resource, face, level, locked, rect, flags): D3DLOCK_NOOVERWRITE skips the block above, yet a
// title may read what the GPU drew
HLE_WRAP(Lock2DSurface, 6) {
    uint32_t res = a[0];
    if ((a[5] & 0x20) && res && g_backend) {  // D3DLOCK_NOOVERWRITE
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        g_backend->SyncLate(mem::VirtualFromPhysical(R32(res + 4)));
    }
}
HLE_STDCALL(D3DResource_BlockUntilNotBusy, 1) {
    if (a[0] && g_backend) {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        g_backend->WaitResource(a[0]);
    }
    gpu::MarkDirty(a[0]);
    return 0;
}
HLE_STDCALL(D3DResource_IsBusy, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));  // what the title pushed itself before this call comes first
    uint32_t res = a[0];
    if (!res) return 0;
    // set on the device, a surface also through its texture
    uint32_t common = R32(res), parent = (common & 0x70000) == 0x50000 ? R32(res + 0x14) : 0;
    if ((common & 0x780000) || (parent && (R32(parent) & 0x780000))) return 1;  // internal reference count
    return g_backend && g_backend->Busy(res) ? 1 : 0;
}
HLE_STDCALL(D3D_BlockOnTime, 2) { return 0; }
HLE_STDCALL(D3DDevice_BlockOnFence, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));  // what the title pushed itself before this call comes first
    if (g_backend) g_backend->WaitFence(a[0]);
    return 0;
}
HLE_STDCALL(D3DDevice_InsertFence, 0) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));  // what the title pushed itself before this call comes first
    return g_backend ? g_backend->Fence() : 0;
}
HLE_STDCALL(D3DDevice_IsFencePending, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));  // what the title pushed itself before this call comes first
    return g_backend && g_backend->FencePending(a[0]) ? 1 : 0;
}
HLE_STDCALL(D3D_SetFence, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));  // what the title pushed itself before this call comes first
    return g_backend ? g_backend->Fence() : 0;
}
HLE_STDCALL(D3D_KickOffAndWaitForIdle, 0) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));  // what the title pushed itself before this call comes first
    if (g_backend) g_backend->Flush();
    return 0;
}
HLE_FASTCALL(CDevice_KickOff, 0) { return 0; }

HLE_STDCALL(D3DDevice_MakeSpace, 0) {
    LOG_FIRST(16, "d3d: MakeSpace from %08X, some caller still pushes hardware commands", R32(ctx.esp));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    return ScratchPush();
}
HLE_STDCALL(D3D_MakeRequestedSpace, 2) {
    LOG_FIRST(16, "d3d: MakeRequestedSpace from %08X", R32(ctx.esp));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    return ScratchPush();
}

HLE_STDCALL(D3DDevice_BlockUntilVerticalBlank, 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
    return 0;
}
HLE_STDCALL(D3DDevice_SetFlickerFilter, 1) { return 0; }
HLE_STDCALL(D3DDevice_SetSoftDisplayFilter, 1) { return 0; }
HLE_STDCALL(D3DDevice_EnableOverlay, 1) { return 0; }
// the front buffer, read back at the guest size, is kept for the next instance after a quick reboot, which may
// fade from it at its first frames
HLE_STDCALL(D3DDevice_PersistDisplay, 0) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));  // what the title pushed itself before this call comes first
    uint32_t front = g_buffers[0], size = R32(front + 0x10);
    uint32_t pitch = ((size >> 24 & 0xFF) + 1) * 64, height = (size >> 12 & 0xFFF) + 1, bytes = pitch * height;
    uint32_t data = mem::VirtualFromPhysical(R32(front + 4));
    if (g_backend) g_backend->Sync(data, bytes);
    kernel::SavePersistedDisplay(R32(front), R32(front + 0xC), size, HostPtr(data), bytes);
    LOGI("d3d: PersistDisplay, %u bytes", bytes);
    return 0;
}
HLE_STDCALL(D3DDevice_FlushVertexCache, 0) { return 0; }

HLE_STDCALL(D3DDevice_SetGammaRamp, 2) {
    uint8_t ramp[768];
    std::memcpy(ramp, HostPtr(a[1]), 768);
    if (g_backend) g_backend->SetGamma(ramp);
    return 0;
}

// targets and presentation

HLE_STDCALL(D3DDevice_SetRenderTarget, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));  // what the title pushed itself before this call comes first
    uint32_t rt = a[0] ? a[0] : Dev(kDevRenderTarget), zs = a[1];
    if (rt && Watched(R32(rt + 4))) LOG_FIRST(40, "watch: SetRenderTarget(%08X) data %08X", rt, R32(rt + 4));
    AddRef(ctx, rt);
    uint32_t old_rt = Dev(kDevRenderTarget), old_zs = Dev(kDevDepth);
    SetDev(kDevRenderTarget, rt);
    SetDev(kDevDepth, zs);
    AddRef(ctx, zs);
    Release(ctx, old_rt);
    Release(ctx, old_zs);
    g_state.rt = SurfaceOf(rt);
    g_state.zs = SurfaceOf(zs);
    // SetRenderTarget resets the viewport to the whole target, and with it the hardware clip the scissors had set
    // (the library keeps no scissors of its own: SetScissors writes the clip, the viewport code writes it again)
    uint32_t w, h;
    gpu::SurfaceSize(g_state.rt, w, h);
    g_state.viewport = {0, 0, w, h, 0.0f, 1.0f};
    g_state.scissor_count = 0;
    UpdateViewportConstants();
    return 0;
}

HLE_STDCALL(D3DDevice_GetBackBuffer2, 1) {
    int32_t i = int32_t(a[0]);
    uint32_t s = i < 0 ? g_buffers[0] : g_buffers[std::min<uint32_t>(uint32_t(i) + 1, g_buffer_count - 1)];
    W32(s, R32(s) + 1);
    return s;
}

HLE_STDCALL(D3DDevice_GetRenderTarget2, 0) {
    uint32_t s = Dev(kDevRenderTarget);
    if (s) W32(s, R32(s) + 1);
    return s;
}

HLE_STDCALL(D3DDevice_GetDepthStencilSurface2, 0) {
    uint32_t s = Dev(kDevDepth);
    if (s) W32(s, R32(s) + 1);
    return s;
}

namespace {
std::atomic<uint32_t> g_presents{0};
}  // namespace

HLE_STDCALL(D3DDevice_Swap, 1) {
    app::SplashFirstFrame();
    // handed over from the previous instance: the window opens on the second frame, the first picture with
    // anything drawn over it right after
    if (app::HandingOver() && g_presents >= 2) {
        {
            std::lock_guard<std::recursive_mutex> lock(g_lock);
            if (g_backend) g_backend->Flush();
        }
        app::FinishHandover();
    }
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_presents++;
    // the kept display is shown until the title presents its own
    kernel::SetSavedDataAddress(0);
    // the ramp an inlined SetGammaRamp left for the vertical blank that shows this swap
    if (g_backend && XDK_CDevice_GammaPending != xdk::kUnknown && XDK_CDevice_GammaRamps != xdk::kUnknown) {
        uint32_t i = Dev(kDevSwapCount) & 1;
        if (Dev(XDK_CDevice_GammaPending + i * 4) == 1) {
            SetDev(XDK_CDevice_GammaPending + i * 4, 0);
            g_backend->SetGamma(static_cast<const uint8_t*>(HostPtr(kDevice + XDK_CDevice_GammaRamps + i * 0x300)));
        }
    }
    Walk(Dev(kDevPush));  // reports asked for at the end of the frame
    if (g_backend) g_backend->Present(SurfaceOf(g_buffers[1]));
    app::Pace();
    // like the XDK, the surfaces stay put and their memory rotates: titles keep back buffer pointers and copy its data
    uint32_t last = g_buffers[g_buffer_count - 1], data = R32(last + 4), lk = R32(last + 8);
    for (uint32_t i = g_buffer_count - 1; i > 0; i--) {
        W32(g_buffers[i] + 4, R32(g_buffers[i - 1] + 4));
        W32(g_buffers[i] + 8, R32(g_buffers[i - 1] + 8));
    }
    W32(g_buffers[0] + 4, data);
    W32(g_buffers[0] + 8, lk);
    g_state.rt = SurfaceOf(Dev(kDevRenderTarget));
    uint32_t n = Dev(kDevSwapCount) + 1;
    SetDev(kDevSwapCount, n);
    return n;
}

HLE_STDCALL(D3DDevice_Clear, 6) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));  // what the title pushed itself before this call comes first
    const uint8_t* rects = a[0] && a[1] ? static_cast<const uint8_t*>(HostPtr(a[1])) : nullptr;
    if (g_backend) g_backend->Clear(g_state, a[0], rects, a[2], a[3], ArgF(a, 4), a[5]);
    return 0;
}

HLE_STDCALL(D3DDevice_CopyRects, 5) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));  // what the title pushed itself before this call comes first
    if (g_backend) g_backend->Copy(SurfaceOf(a[0]), SurfaceOf(a[3]), a[2], a[1], a[4]);
    return 0;
}

HLE_STDCALL(D3DDevice_SetViewport, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));  // what the title pushed itself before this call comes first
    uint32_t v = a[0];
    // the library keeps the viewport inside the render target: right and bottom stop at its edges (0x539710)
    uint32_t tw, th;
    gpu::SurfaceSize(g_state.rt.header ? g_state.rt : g_state.zs, tw, th);
    uint32_t x = R32(v), y = R32(v + 4);
    uint32_t right = uint32_t(std::min<uint64_t>(uint64_t(x) + R32(v + 8), tw));
    uint32_t bottom = uint32_t(std::min<uint64_t>(uint64_t(y) + R32(v + 12), th));
    g_state.viewport = {x, y, right > x ? right - x : 0, bottom > y ? bottom - y : 0, GF(v + 16), GF(v + 20)};
    g_state.scissor_count = 0;
    UpdateViewportConstants();
    return 0;
}

HLE_STDCALL(D3DDevice_SetScissors, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));  // what the title pushed itself before this call comes first
    g_state.scissor_count = std::min<uint32_t>(a[0], 8);
    for (uint32_t i = 0; i < g_state.scissor_count * 4; i++) g_state.scissors[i] = R32(a[2] + i * 4);
    return 0;
}

HLE_STDCALL(D3DDevice_SetScreenSpaceOffset, 2) {
    g_state.ss_offset_x = ArgF(a, 0);
    g_state.ss_offset_y = ArgF(a, 1);
    UpdateViewportConstants();
    return 0;
}

// render states

HLE_FASTCALL(D3DDevice_SetRenderState_Simple, 0) {
    uint32_t method = a[0] & 0x1FFC, value = a[1];
    g_state.simple[method >> 2] = value;
    return 0;
}

HLE_STDCALL(D3DDevice_SetRenderStateNotInline, 2) {
    uint32_t rs = a[0], v = a[1];
    if (rs < gpu::kRsDeferredFirst) {
        uint32_t method = R32(kSimpleEncode + rs * 4) & 0x1FFC;
        g_state.simple[method >> 2] = v;
        W32(kRenderState + rs * 4, v);  // titles read their states back from the library's table
    } else if (rs < gpu::kRsMax) {
        SetComplex(rs, v);
    }
    return 0;
}

#define COMPLEX_STATE(name, rs)                       \
    HLE_STDCALL(D3DDevice_SetRenderState_##name, 1) { \
        SetComplex(rs, a[0]);                         \
        return 0;                                     \
    }
COMPLEX_STATE(PSTextureModes, gpu::kRsPsTextureModes)
COMPLEX_STATE(VertexBlend, gpu::kRsVertexBlend)
COMPLEX_STATE(FogColor, gpu::kRsFogColor)
COMPLEX_STATE(FillMode, gpu::kRsFillMode)
COMPLEX_STATE(BackFillMode, gpu::kRsBackFillMode)
COMPLEX_STATE(TwoSidedLighting, gpu::kRsTwoSidedLighting)
COMPLEX_STATE(NormalizeNormals, gpu::kRsNormalizeNormals)
COMPLEX_STATE(ZEnable, gpu::kRsZEnable)
COMPLEX_STATE(StencilEnable, gpu::kRsStencilEnable)
COMPLEX_STATE(StencilFail, gpu::kRsStencilFail)
COMPLEX_STATE(FrontFace, gpu::kRsFrontFace)
COMPLEX_STATE(CullMode, gpu::kRsCullMode)
COMPLEX_STATE(TextureFactor, gpu::kRsTextureFactor)
COMPLEX_STATE(LogicOp, gpu::kRsLogicOp)
COMPLEX_STATE(EdgeAntiAlias, gpu::kRsEdgeAntiAlias)
COMPLEX_STATE(MultiSampleAntiAlias, gpu::kRsMultiSampleAntiAlias)
COMPLEX_STATE(MultiSampleMask, gpu::kRsMultiSampleMask)
COMPLEX_STATE(MultiSampleMode, gpu::kRsMultiSampleMode)
COMPLEX_STATE(MultiSampleRenderTargetMode, gpu::kRsMultiSampleRenderTargetMode)
COMPLEX_STATE(ShadowFunc, gpu::kRsShadowFunc)
COMPLEX_STATE(LineWidth, gpu::kRsLineWidth)
COMPLEX_STATE(SampleAlpha, gpu::kRsSampleAlpha)
COMPLEX_STATE(Dxt1NoiseEnable, gpu::kRsDxt1NoiseEnable)
COMPLEX_STATE(YuvEnable, gpu::kRsYuvEnable)
COMPLEX_STATE(OcclusionCullEnable, gpu::kRsOcclusionCullEnable)
COMPLEX_STATE(StencilCullEnable, gpu::kRsStencilCullEnable)
COMPLEX_STATE(RopZCmpAlwaysRead, gpu::kRsRopZCmpAlwaysRead)
COMPLEX_STATE(RopZRead, gpu::kRsRopZRead)
COMPLEX_STATE(DoNotCullUncompressed, gpu::kRsDoNotCullUncompressed)

HLE_STDCALL(D3DDevice_SetTextureState_TexCoordIndex, 2) {
    g_state.tss[a[0] & 3][gpu::kTssTexCoordIndex] = a[1];
    W32(kTextureState + ((a[0] & 3) * kStageStates + gpu::kTssTexCoordIndex) * 4, a[1]);
    return 0;
}
HLE_STDCALL(D3DDevice_SetTextureState_BumpEnv, 3) {
    if (a[1] >= gpu::kTssBumpEnvMat00 && a[1] <= gpu::kTssBumpEnvLOffset) {
        g_state.tss[a[0] & 3][a[1]] = a[2];
        W32(kTextureState + ((a[0] & 3) * kStageStates + a[1]) * 4, a[2]);
    }
    return 0;
}
HLE_STDCALL(D3DDevice_SetTextureState_BorderColor, 2) {
    g_state.tss[a[0] & 3][gpu::kTssBorderColor] = a[1];
    g_state.simple[(0x1B24 + (a[0] & 3) * 0x40) >> 2] = a[1];
    return 0;
}
HLE_STDCALL(D3DDevice_SetTextureState_ColorKeyColor, 2) {
    g_state.tss[a[0] & 3][gpu::kTssColorKeyColor] = a[1];
    return 0;
}
HLE_STDCALL(D3DDevice_SetTextureStageStateNotInline, 3) {
    uint32_t stage = a[0] & 3, type = a[1], v = a[2];
    if (type < gpu::kTssCount) {
        g_state.tss[stage][type] = v;
        W32(kTextureState + (stage * kStageStates + type) * 4, v);
    }
    return 0;
}

// the XDK's transform types: view 0, projection 1, texture 2-5, world 6-9; the renderer keeps Direct3D 8's numbers
// (view 2, projection 3, texture 16, world 256). The device copy is what GetTransform reads.
HLE_STDCALL(D3DDevice_SetTransform, 2) {
    uint32_t t = a[0];
    if (t >= 10) return 0;
    std::memcpy(HostPtr(kDevice + kDevTransforms + t * 64), HostPtr(a[1]), 64);
    uint32_t pc = t < 2 ? t + 2 : t < 6 ? t + 14 : t + 250;
    std::memcpy(g_state.transforms[pc].data(), HostPtr(a[1]), 64);
    return 0;
}

HLE_STDCALL(D3DDevice_SetMaterial, 1) {
    std::memcpy(g_state.material.data(), HostPtr(a[0]), 17 * 4);
    return 0;
}

// the XDK keeps its own light lists, the renderer gets a copy
HLE_WRAP(D3DDevice_SetLight, 2) {
    uint32_t i = a[0], l = a[1];
    if (i < 8 && l) {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        std::memcpy(g_state.lights[i].data(), HostPtr(l), 26 * 4);
    }
}
HLE_WRAP(D3DDevice_LightEnable, 2) {
    uint32_t i = a[0], on = a[1];
    if (i < 8) {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        g_state.light_enable = on ? g_state.light_enable | 1u << i : g_state.light_enable & ~(1u << i);
    }
}

// resources in use

HLE_STDCALL(D3DDevice_SetTexture, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));  // what the title pushed itself before this call comes first
    uint32_t stage = a[0] & 3, tex = a[1];
    if (tex && Watched(R32(tex + 4))) LOG_FIRST(40, "watch: SetTexture(%u, %08X) data %08X from %08X", stage, tex, R32(tex + 4), R32(ctx.esp));
    uint32_t old = Dev(kDevTextures + stage * 4);
    AddRef(ctx, tex);
    SetDev(kDevTextures + stage * 4, tex);
    Release(ctx, old);
    g_state.textures[stage] = tex;
    W32(kDirtyFlags, R32(kDirtyFlags) | XDK_DirtyFlags_Texture);
#if XDK_CDevice_PixelShader != 0xFFFFFFFFu
    // the stage's format bits (+0xC, 0x539960), which the texture modes (0x541C50) read: none, cube, 3D or a
    // depth format (bit 30, formats 0x2A-0x31 with bit 13 set)
    {
        uint32_t info = 0x80000000u;
        if (tex) {
            uint32_t format = R32(tex + 12);
            info = format & 0x20F4;
            if (info & 0x2000) {
                info &= ~0x2000u;
                if ((format & 0xFF00) >= 0x2A00 && (format & 0xFF00) <= 0x3100) info |= 0x40000000u;
            }
        }
        SetDev(0xC + stage * 4, info);
    }
    // and the methods it sends: offset and format, a linear texture's pitch and size, the stage turned off or back on
    // with the control word LazySetStates last sent (+0x774). Push buffers the title records leave the textures out
    // and draw with these
    {
        uint32_t m = 0x1B00 + stage * 0x40;
        if (!tex) {
            g_state.simple[(m + 0xC) >> 2] = 0;
        } else {
            g_state.simple[m >> 2] = R32(tex + 4);
            g_state.simple[(m + 4) >> 2] = R32(tex + 12);
            if (uint32_t size = R32(tex + 16)) {
                g_state.simple[(m + 0x10) >> 2] = (((size >> 24) + 1) * 64) << 16;
                g_state.simple[(m + 0x1C) >> 2] = ((size & 0xFFF) + 1) << 16 | (((size >> 12) & 0xFFF) + 1);
            }
            if (!old) g_state.simple[(m + 0xC) >> 2] = Dev(0x774 + stage * 4);
        }
    }
#endif
    return 0;
}

// SetPalette(stage, palette): the colours a P8 texture of the stage looks up
HLE_STDCALL(D3DDevice_SetPalette, 2) {
    XDK_REQUIRE("d3d", XDK_KEY(CDevice_Palettes));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t stage = a[0] & 3, pal = a[1];
    uint32_t old = Dev(kDevPalettes + stage * 4);
    AddRef(ctx, pal);
    SetDev(kDevPalettes + stage * 4, pal);
    Release(ctx, old);
    g_state.palettes[stage] = pal;
    return 0;
}

// SwitchTexture(method, data, format), fastcall: a swizzled texture of the bound one's shape replaces it. It is
// bound through a header of the stage's own holding the bound texture's fields with the new data and format.
HLE_FASTCALL(D3DDevice_SwitchTexture, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));  // what the title pushed itself before this call comes first
    static uint32_t headers[4];
    // texture methods from 0x1B00, 0x40 per stage
    uint32_t stage = (((a[0] & 0x1FFC) - 0x1B00) >> 6) & 3, cur = g_state.textures[stage];
    uint32_t& h = headers[stage];
    if (!h) h = mem::AllocSystem(20);
    if (cur && cur != h) std::memcpy(HostPtr(h), HostPtr(cur), 20);
    W32(h + 4, a[1]);
    W32(h + 12, a[2]);
    g_state.textures[stage] = h;
    return 0;
}

HLE_STDCALL(D3DDevice_SetStreamSource, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));  // what the title pushed itself before this call comes first
    uint32_t n = a[0] & 15, vb = a[1], stride = a[2];
    uint32_t old = R32(kStreams + n * kStreamSize + XDK_Stream_Buffer);
    AddRef(ctx, vb);
    W32(kStreams + n * kStreamSize + XDK_Stream_Stride, stride);
    W32(kStreams + n * kStreamSize + XDK_Stream_Buffer, vb);
    Release(ctx, old);
    g_state.streams[n] = {vb, stride};
    return 0;
}

HLE_STDCALL(D3DDevice_SetIndices, 2) {
    XDK_REQUIRE("d3d", XDK_DATA(D3D_IndexData));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t ib = a[0];
    uint32_t old = Dev(kDevIndexBuffer);
    AddRef(ctx, ib);
    W32(kIndexData, ib ? R32(ib + 4) : 0);
    SetDev(kDevIndexBuffer, ib);
    SetDev(kDevBaseVertex, a[1]);
    Release(ctx, old);
    g_state.index_buffer = ib;
    g_state.index_data = ib ? R32(ib + 4) : 0;
    g_state.base_vertex = a[1];
    return 0;
}

// shaders

HLE_STDCALL(D3DDevice_CreateVertexShader, 4) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));  // what the title pushed itself before this call comes first
    uint32_t decl = a[0], func = a[1], out = a[2];
    auto vs = std::make_shared<gpu::VertexShader>();
    if (decl) {
        for (uint32_t i = 0; i < 1024; i++) {
            uint32_t t = R32(decl + i * 4);
            vs->decl.push_back(t);
            if (t == 0xFFFFFFFF) break;
        }
    }
    if (func) {
        uint32_t hdr = R32(func);
        vs->program_type = hdr & 0xFFFF;
        uint32_t n = hdr >> 16;
        for (uint32_t i = 0; i < n * 4; i++) vs->program.push_back(R32(func + 4 + i * 4));
    }
    gpu::ParseDeclaration(*vs);
    uint32_t rec = mem::AllocSystem(16, 16);
    vs->handle = rec | 1;
    g_vs[vs->handle] = vs;
    W32(out, vs->handle);
    LOG_FIRST(64, "d3d: vertex shader %08X, %zu decl tokens, %zu instructions, type %X", vs->handle, vs->decl.size(),
              vs->program.size() / 4, vs->program_type);
    return 0;
}

HLE_STDCALL(D3DDevice_DeleteVertexShader, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));  // what the title pushed itself before this call comes first
    auto it = g_vs.find(a[0]);
    if (it != g_vs.end()) {
        mem::FreeSystem(a[0] & ~1u);
        g_vs.erase(it);
    }
    return 0;
}

// (handle, data, size in and out): the program as it was given, the size alone without data or room
HLE_STDCALL(D3DDevice_GetVertexShaderFunction, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));  // what the title pushed itself before this call comes first
    auto it = g_vs.find(a[0]);
    uint32_t words = it == g_vs.end() ? 0 : uint32_t(it->second->program.size());
    uint32_t need = words ? 4 + words * 4 : 0, room = R32(a[2]);
    W32(a[2], need);
    if (need > room) return a[1] ? 0x88760867 : 0;  // D3DERR_MOREDATA
    if (!a[1] || !need) return 0;
    W32(a[1], it->second->program_type | words / 4 << 16);
    for (uint32_t i = 0; i < words; i++) W32(a[1] + 4 + i * 4, it->second->program[i]);
    return 0;
}

HLE_STDCALL(D3DDevice_SetVertexShader, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));  // what the title pushed itself before this call comes first
    uint32_t h = a[0];
    g_state.vs_handle = h;
    g_state.vs = nullptr;
    if (h & 1) {
        auto it = g_vs.find(h);
        if (it != g_vs.end()) {
            g_state.vs = it->second;
            // the program goes to the GPU's program memory from 0 and runs from there, as the XDK loads it: push buffers
            // run after this draw with it
            if (!it->second->program.empty()) {
                pb::LoadProgram(it->second->program, 0);
                g_state.simple[0x1EA0 >> 2] = 0;
            }
            // constants declared with the shader load when it is selected
            for (auto& [reg, v] : it->second->constants)
                if (reg < 192) g_state.vsc[reg] = v;
        } else {
            LOG_FIRST(8, "d3d: SetVertexShader with unknown handle %08X", h);
        }
    }
    return 0;
}

HLE_FASTCALL(D3DDevice_SetVertexShaderConstant1Fast, 0) {
    uint32_t reg = a[0];
    if (reg < 192) std::memcpy(g_state.vsc[reg].data(), HostPtr(a[1]), 16);
    return 0;
}

HLE_FASTCALL(D3DDevice_SetVertexShaderConstant1, 0) {
    uint32_t reg = a[0];
    if (reg < 192) std::memcpy(g_state.vsc[reg].data(), HostPtr(a[1]), 16);
    return 0;
}

HLE_FASTCALL(D3DDevice_SetVertexShaderConstant4, 0) {
    uint32_t reg = a[0], src = a[1];
    for (uint32_t i = 0; i < 4 && reg + i < 192; i++) std::memcpy(g_state.vsc[reg + i].data(), HostPtr(src + i * 16), 16);
    return 0;
}

HLE_FASTCALL(D3DDevice_SetVertexShaderConstantNotInlineFast, 1) {
    uint32_t reg = a[0], src = a[1], dwords = a[2];
    for (uint32_t i = 0; i < dwords / 4 && reg + i < 192; i++) std::memcpy(g_state.vsc[reg + i].data(), HostPtr(src + i * 16), 16);
    return 0;
}

HLE_STDCALL(D3DDevice_LoadVertexShader, 2) {
    LOG_FIRST(4, "d3d: LoadVertexShader %08X at %u is not supported", a[0], a[1]);
    return 0;
}
// SelectVertexShader(handle, address): the program LoadVertexShader (or the title's own push) put in the GPU's program
// memory at address, read with the handle's attributes: a shader the host made, else the object's attribute table at
// +0x14 (16 inputs of stream, offset, D3DVSDT type, as SelectVertexShaderDirect fills the library's own object)
HLE_STDCALL(D3DDevice_SelectVertexShader, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));  // what the title pushed itself before this call comes first
    static std::unordered_map<uint64_t, std::shared_ptr<gpu::VertexShader>> made;
    uint32_t handle = a[0], address = a[1] & 0xFF;
    std::vector<uint32_t> prog = pb::ProgramFrom(address);
    auto known = g_vs.find(handle);
    uint32_t table[64] = {};
    if (known == g_vs.end() && handle) std::memcpy(table, HostPtr((handle & ~1u) + 0x14), sizeof(table));
    uint64_t key = pb::Fnv(table, sizeof(table), pb::Fnv(prog.data(), prog.size() * 4, handle * 0x9E3779B97F4A7C15ull));
    auto& vs = made[key];
    if (!vs) {
        vs = std::make_shared<gpu::VertexShader>();
        vs->handle = 0x44000001u + uint32_t(made.size()) * 2;
        vs->program = std::move(prog);
        vs->program_type = 0x2078;
        if (known != g_vs.end()) {
            vs->inputs = known->second->inputs;
            vs->stream_stride = known->second->stream_stride;
            vs->decl = known->second->decl;
        } else {
            for (int i = 0; i < 16; i++) {
                uint32_t stream = table[i * 4], offset = table[i * 4 + 1], type = table[i * 4 + 2];
                if (type == 0x02 || !type) continue;  // D3DVSDT_NONE
                vs->inputs[size_t(i)] = {int(stream & 15), offset, type};
            }
        }
    }
    g_state.vs = vs;
    g_state.vs_handle = vs->handle;
    g_state.simple[0x1EA0 >> 2] = address;  // the program start the library pushes
    SetDev(0x794, handle ? handle - 1 : 0);  // the device's current shader object, which its inlined code reads
    return 0;
}
HLE_STDCALL(D3DDevice_SetShaderConstantMode, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));  // what the title pushed itself before this call comes first
    g_own_constants = a[0] & 0x10;  // D3DSCM_NORESERVEDCONSTANTS
    UpdateViewportConstants();
    return 0;
}

HLE_STDCALL(D3DDevice_CreatePixelShader, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));  // what the title pushed itself before this call comes first
    auto ps = std::make_shared<gpu::PixelShader>();
    std::memcpy(ps->def, HostPtr(a[0]), sizeof(ps->def));
    uint32_t rec = mem::AllocSystem(16, 16);
    ps->handle = rec;
    g_ps[rec] = ps;
    W32(a[1], rec);
    return 0;
}

HLE_STDCALL(D3DDevice_DeletePixelShader, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));  // what the title pushed itself before this call comes first
    if (g_ps.erase(a[0])) mem::FreeSystem(a[0]);
    return 0;
}

HLE_STDCALL(D3DDevice_SetPixelShader, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));  // what the title pushed itself before this call comes first
    g_state.ps = nullptr;
    // the device keeps what LazySetStates reads (0x538690): the shader (+0x784), whether it has a final combiner
    // (+0x788), its texture modes (+0x790), with the combiners, texture modes and fog marked dirty as the library does.
    // Without it a push buffer run after SetPixelShader(0) finds no fixed function combiners sent and draws with
    // the last shader's
#if XDK_CDevice_PixelShader != 0xFFFFFFFFu
    {
        constexpr uint32_t kFinalCombiner = XDK_CDevice_PixelShader + 4, kTextureModes = XDK_CDevice_PixelShader + 0xC;
        constexpr uint32_t kDirtyCombiners = 0x800, kDirtyTextureModes = 0x4000, kDirtyFog = 0x2000;
        uint32_t flags = R32(kDirtyFlags);
        SetDev(XDK_CDevice_PixelShader, a[0]);
        if (!a[0]) {
            flags |= kDirtyCombiners | kDirtyTextureModes;
            if (Dev(kFinalCombiner)) flags |= kDirtyFog;
        } else if (const uint32_t def = R32(a[0] + 8)) {
            uint32_t final_combiner = R32(def + 0x20) | R32(def + 0x24);
            if (!final_combiner && Dev(kFinalCombiner)) flags |= kDirtyFog;
            SetDev(kFinalCombiner, final_combiner);
            SetDev(XDK_CDevice_PixelShader + 8, R32(def + 0xEC) & 0x100);
            SetDev(kTextureModes, R32(def + 0xD8));
            flags |= kDirtyTextureModes;
        }
        W32(kDirtyFlags, flags);
    }
#endif
    if (!a[0]) return 0;
    auto it = g_ps.find(a[0]);
    if (it != g_ps.end()) {
        g_state.ps = it->second;
    } else if (XDK_PixelShader_Definition != xdk::kUnknown) {
        // made by library code the title inlined: the object points at its definition, handles are reused
        static std::unordered_map<uint32_t, std::shared_ptr<gpu::PixelShader>> made;
        const void* def = HostPtr(R32(a[0] + XDK_PixelShader_Definition));
        auto& ps = made[a[0]];
        if (!ps || std::memcmp(ps->def, def, sizeof(ps->def))) {
            ps = std::make_shared<gpu::PixelShader>();
            ps->handle = a[0];
            std::memcpy(ps->def, def, sizeof(ps->def));
        }
        g_state.ps = ps;
    } else {
        LOG_FIRST(8, "d3d: SetPixelShader with unknown handle %08X", a[0]);
        return 0;
    }
    // the definition loads the combiner state like the XDK pushes it
    const uint32_t* d = g_state.ps->def;
    auto put = [&](uint32_t method, uint32_t v) { g_state.simple[method >> 2] = v; };
    for (int i = 0; i < 8; i++) {
        put(gpu::kMPsAlphaInputs + i * 4, d[0 + i]);
        put(gpu::kMPsConstant0 + i * 4, d[10 + i]);
        put(gpu::kMPsConstant1 + i * 4, d[18 + i]);
        put(gpu::kMPsAlphaOutputs + i * 4, d[26 + i]);
        put(gpu::kMPsRgbInputs + i * 4, d[34 + i]);
        put(gpu::kMPsRgbOutputs + i * 4, d[45 + i]);
    }
    put(gpu::kMPsFinalAbcd, d[8]);
    put(gpu::kMPsFinalEfgh, d[9]);
    put(gpu::kMPsCompareMode, d[42]);
    put(gpu::kMPsFinalConstant0, d[43]);
    put(gpu::kMPsFinalConstant1, d[44]);
    put(gpu::kMPsCombinerCount, d[53]);
    put(gpu::kMPsDotMapping, d[55]);
    put(gpu::kMPsInputTexture, d[56]);
    put(0x1E70, d[54]);  // the shader stage program, as the XDK loads it too: push buffers run with it
    SetComplex(gpu::kRsPsTextureModes, d[54]);
    return 0;
}

HLE_STDCALL(D3DDevice_SetPixelShaderConstant, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));  // what the title pushed itself before this call comes first
    uint32_t reg = a[0], src = a[1], n = a[2];
    for (uint32_t i = 0; i < n && reg + i < 16; i++) std::memcpy(g_state.psc[reg + i].data(), HostPtr(src + i * 16), 16);
    gpu::ApplyPixelConstants(g_state, reg, n);
    return 0;
}

// drawing

HLE_STDCALL(D3DDevice_DrawVertices, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));  // a visibility test begun since the last draw counts this one
    SyncDeferred();
    if (g_state.rt.header && Watched(g_state.rt.data))
        LOG_FIRST(10, "watch: DrawVertices(%u, %u, %u) stream0 %08X/%u, library's %08X/%u, from %08X", a[0], a[1], a[2],
                  g_state.streams[0].vb, g_state.streams[0].stride, R32(kStreams + XDK_Stream_Buffer), R32(kStreams + XDK_Stream_Stride), R32(ctx.esp));
    if (g_backend) g_backend->Draw(g_state, a[0], a[1], a[2], nullptr);
    return 0;
}

HLE_STDCALL(D3DDevice_DrawIndexedVertices, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    SyncDeferred();
    const uint8_t* indices = a[2] ? static_cast<const uint8_t*>(HostPtr(a[2])) : nullptr;
    if (g_backend) g_backend->Draw(g_state, a[0], 0, a[1], indices);
    return 0;
}

// DrawVerticesUP(prim, count, vertices, stride): the vertices are copied to a ring the render thread reads, stream 0
// points there through a vertex buffer header for the draw. A half of the ring is written again once the draws
// that read it are done.
namespace {
constexpr uint32_t kUpRing = 32u << 20, kUpHalf = kUpRing / 2;  // push buffer draws copy their vertices here too
uint32_t g_up_ring = 0, g_up_pos = 0, g_up_header = 0, g_up_fence[2] = {};

uint32_t UpVertices(const void* src, uint32_t bytes) {
    if (!g_up_ring) {
        g_up_ring = mem::AllocContiguous(kUpRing, 0, 0xFFFFFFFF, 0x1000);
        g_up_header = mem::AllocSystem(20);
        std::memset(HostPtr(g_up_header), 0, 20);
        W32(g_up_header, 1);  // a vertex buffer, one reference
    }
    bytes = std::min(bytes, kUpHalf);
    uint32_t room = (bytes + 63) & ~63u;
    uint32_t pos = g_up_pos + room > kUpRing ? 0 : g_up_pos;
    uint32_t from = g_up_pos ? (g_up_pos - 1) / kUpHalf : 1, to = (pos + room - 1) / kUpHalf;
    if (to != from) {
        g_up_fence[from] = g_backend->Fence();
        g_backend->WaitFence(g_up_fence[to]);
    }
    std::memcpy(HostPtr(g_up_ring + pos), src, bytes);
    g_up_pos = pos + room;
    W32(g_up_header + 4, mem::PhysicalFromVirtual(g_up_ring + pos));
    return g_up_header;
}

void DrawUp(uint32_t prim, uint32_t count, const void* vertices, uint32_t stride) {
    if (!g_backend || !count || !stride) return;
    if (g_state.vs && (g_state.vs_handle & 1)) {
        uint32_t need = 0;
        for (const auto& in : g_state.vs->inputs)
            if (in.stream == 0) need = std::max<uint32_t>(need, in.offset + 4);
        if (need > stride)
            LOG_FIRST(20, "d3d: up draw prim %u count %u stride %u, layout reaches %u (vs %08X, stream stride %u)", prim, count,
                      stride, need, g_state.vs_handle, g_state.vs->stream_stride[0]);
    }
    SyncDeferred();
    auto stream = g_state.streams[0];
    g_state.streams[0] = {UpVertices(vertices, count * stride), stride};
    g_backend->Draw(g_state, prim, 0, count, nullptr);
    g_state.streams[0] = stream;
}

// bytes per vertex of the bound format: stream 0 of the FVF or of the shader's declaration
uint32_t VertexBytes() {
    if (g_state.vs_handle & 1) return g_state.vs ? g_state.vs->stream_stride[0] : 0;
    gpu::VertexShader fvf;
    gpu::FvfInputs(g_state.vs_handle, fvf);
    return fvf.stream_stride[0];
}

uint32_t g_push_begin = 0;
}  // namespace

// Begin/End immediate mode. Titles inline Begin and the vertex data calls, so their commands are read from the scratch buffer.

namespace d3d {
namespace {
uint32_t g_im_prim = 0, g_im_mask = 0;  // open primitive, slots it wrote
float g_im_attr[64];                    // 16 slots of x y z w
std::vector<float> g_im_vertices;       // every slot per vertex

// visibility tests: depth passing pixels are counted while enabled, a report gets the count since the clear,
// at a physical address in the first 16 MB
void ApplyVisibilityMethod(uint32_t m, uint32_t v) {
    constexpr uint32_t kReportAddressMask = 0xFFFFFF;
    if (m == gpu::kMCountEnable) g_state.simple[gpu::kMCountEnable >> 2] = v;
    if (!g_backend) return;
    if (m == gpu::kMClearReport) g_backend->ClearReport();
    if (m == gpu::kMGetReport) g_backend->GetReport(mem::VirtualFromPhysical(v & kReportAddressMask));
}

// draws the open primitive: the slots it wrote are FLOAT4 inputs of the bound program, or of the fixed pipeline
void EndPrimitive() {
    struct Made {
        std::shared_ptr<gpu::VertexShader> from, vs;
    };
    // the render thread tells shaders apart by pointer: one object per program and slot set, the program kept alive
    static std::map<std::pair<gpu::VertexShader*, uint32_t>, Made> made;
    static std::vector<float> packed;
    uint32_t prim = g_im_prim, count = uint32_t(g_im_vertices.size() / 64), handle = g_state.vs_handle;
    g_im_prim = 0;
    if (!prim || !count) return;
    if (!(handle & 1) && (handle & 0xE) == 4) LOG_FIRST(1, "d3d: pretransformed immediate mode vertices are not supported");
    auto bound = g_state.vs, from = handle & 1 ? bound : nullptr;
    Made& m = made[{from.get(), g_im_mask}];
    if (!m.vs) {
        m.from = from;
        m.vs = std::make_shared<gpu::VertexShader>();
        if (from) {
            m.vs->program = from->program;
            m.vs->program_type = from->program_type;
        }
        for (uint32_t s = 0; s < 16; s++)
            if (g_im_mask >> s & 1) {
                m.vs->inputs[s] = {0, m.vs->stream_stride[0], 0x42};
                m.vs->stream_stride[0] += 16;
            }
    }
    packed.clear();
    for (uint32_t v = 0; v < count; v++)
        for (uint32_t s = 0; s < 16; s++)
            if (g_im_mask >> s & 1) packed.insert(packed.end(), &g_im_vertices[v * 64 + s * 4], &g_im_vertices[v * 64 + s * 4] + 4);
    g_state.vs = m.vs;
    g_state.vs_handle = handle | 1;
    DrawUp(prim, count, packed.data(), m.vs->stream_stride[0]);
    g_state.vs = bound;
    g_state.vs_handle = handle;
}

// reads the commands pushed since the last call: vertex data between a begin and an end is drawn, visibility tests
// are run, the rest is skipped
std::vector<uint32_t> g_inline;  // inline array dwords between a pushed begin and end

void Walk(uint32_t end) {
    if (!g_walk || end < g_walk || end > g_scratch_push + g_scratch_size) return;
    for (uint32_t p = g_walk; p + 4 <= end;) {
        // count in bits 18 to 28, bit 30 keeps the method for every value
        uint32_t head = R32(p), method = head & 0x1FFC, n = (head >> 18) & 0x7FF;
        p += 4;
        if ((head & 0xA0030003) || n * 4 > end - p) {  // anything but a method header
            LOG_FIRST(16, "d3d: walk stops at command %08X at %08X, %u bytes before the end", head, p - 4, end - p);
            break;
        }
        for (uint32_t i = 0; i < n; i++, p += 4) {
            uint32_t m = head & 0x40000000 ? method : method + i * 4, v = R32(p);
            float f, *a = g_im_attr;
            bool last = true;  // the slot's last component
            std::memcpy(&f, &v, 4);
            ApplyVisibilityMethod(m, v);
            if (m == 0x17FC) {  // begin/end, 0 ends
                pb::Method(m, v);   // indexed or ranged array draws the title pushed itself (drawn by pb when it has any)
                if (v) {
                    g_im_prim = v;
                    g_im_mask = 0;
                    g_im_vertices.clear();
                    g_inline.clear();
                } else if (!g_inline.empty()) {
                    // vertices the title pushed whole (BeginPush with its EndPush inlined, link time code generation)
                    uint32_t stride = VertexBytes(), bytes = uint32_t(g_inline.size()) * 4;
                    auto bound = g_state.vs;
                    uint32_t bound_handle = g_state.vs_handle, packed = 0;
                    // the pushed formats, unless they do not divide the data and the bound shader's stride does (formats a
                    // push buffer left that the library did not send again)
                    auto layout = pb::InlineLayout(bound_handle & 1 ? bound : nullptr, packed);
                    if (layout && packed && bytes % packed != 0 && stride && bytes % stride == 0) {
                        LOG_FIRST(20, "d3d: pushed primitive %u: formats of %u bytes do not divide %u, bound stride %u used", g_im_prim, packed,
                                  bytes, stride);
                        layout = nullptr;
                    }
                    if (layout && packed) {
                        g_state.vs = layout;
                        g_state.vs_handle = layout->handle;
                        stride = packed;
                    }
                    if (g_im_prim && stride && bytes % stride == 0) {
                        LOG_FIRST(400, "d3d: pushed primitive %u drawn, %u vertices of %u bytes, vs %08X, t0 %08X", g_im_prim, bytes / stride,
                                  stride, g_state.vs_handle, g_state.textures[0]);
                        DrawUp(g_im_prim, bytes / stride, g_inline.data(), stride);
                    }
                    else LOG_FIRST(200, "d3d: pushed primitive %u dropped, %u bytes of vertices of %u (walk %08X, %08X %08X %08X)", g_im_prim, bytes,
                                  stride, p, g_inline[0], g_inline.size() > 1 ? g_inline[1] : 0, g_inline.size() > 2 ? g_inline[2] : 0);
                    g_state.vs = bound;
                    g_state.vs_handle = bound_handle;
                    g_inline.clear();
                    g_im_prim = 0;
                } else {
                    EndPrimitive();
                }
                continue;
            } else if (m == 0x1800 || m == 0x1808 || m == 0x1810) {  // array elements, array ranges
                pb::Method(m, v);
                continue;
            } else if (m == 0x1818) {  // inline array: the vertices themselves
                if (g_im_prim) g_inline.push_back(v);
                continue;
            } else if (m >= 0x1880 && m < 0x1900) {  // 2 floats, z = 0 and w = 1
                a += (m - 0x1880) / 8 * 4;
                last = (m - 0x1880) / 4 & 1;
                a[last] = f;
                a[2] = 0;
                a[3] = 1;
            } else if (m >= 0x1940 && m < 0x1980) {  // 4 bytes
                a += (m - 0x1940) / 4 * 4;
                for (int c = 0; c < 4; c++) a[c] = (v >> c * 8 & 0xFF) / 255.0f;
            } else if (m >= 0x1A00 && m < 0x1B00) {  // 4 floats
                a += (m - 0x1A00) / 16 * 4;
                last = ((m - 0x1A00) / 4 & 3) == 3;
                a[(m - 0x1A00) / 4 & 3] = f;
            } else if (m >= 0x1500 && m < 0x150C) {  // position, 3 floats and w = 1
                last = m == 0x1508;
                a[(m - 0x1500) / 4] = f;
                a[3] = 1;
            } else if (m >= 0x1518 && m < 0x1528) {  // position, 4 floats
                last = m == 0x1524;
                a[(m - 0x1518) / 4] = f;
            } else {
                // vertex programs and constants load as on the GPU (constant writes the title inlined); any other
                // method the title pushed itself is a simple state, as EndPush takes it
                if ((m >= 0x0B00 && m < 0x0C00) || m == 0x1E9C || m == 0x1EA4) pb::Method(m, v);
                else if (m >> 2 < g_state.simple.size() && !(m >= 0x1800 && m < 0x1B00)) {
                    if (m == 0x2A4) pb::g_fog_seen = true;  // LazySetStates sent the fog: push buffer draws use it
                    g_state.simple[m >> 2] = v;
                }
                continue;
            }
            if (!g_im_prim) continue;
            g_im_mask |= 1u << (a - g_im_attr) / 4;
            if (a == g_im_attr && last) g_im_vertices.insert(g_im_vertices.end(), g_im_attr, g_im_attr + 64);
        }
    }
    g_walk = end;
}
}  // namespace
}  // namespace d3d

HLE_STDCALL(D3DDevice_DrawVerticesUP, 4) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    DrawUp(a[0], a[1], HostPtr(a[2]), a[3]);
    return 0;
}

// BeginPush(dwords) hands out room for raw hardware commands, EndPush(end) runs what the title wrote there:
// a primitive between two begin/end writes with its vertices inline, any other method as a simple state.
HLE_STDCALL(D3DDevice_BeginPush, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));  // what the title pushed itself before this call comes first
    g_push_begin = ScratchPush(a[0] * 4);
    return g_push_begin;
}

HLE_STDCALL(D3DDevice_EndPush, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));  // what the title pushed itself before this call comes first
    constexpr uint32_t kBeginEnd = 0x17FC, kInlineArray = 0x1818;
    static std::vector<uint32_t> vertices;
    uint32_t prim = 0;
    for (uint32_t p = g_push_begin, end = a[0]; g_push_begin && p + 4 <= end;) {
        // count in bits 18 to 28, bit 30 keeps the method for every value
        uint32_t head = R32(p), method = head & 0x1FFC, n = (head >> 18) & 0x7FF;
        p += 4;
        if ((head & 0xA0030003) || n * 4 > end - p) {  // anything but a method header
            LOG_FIRST(8, "d3d: EndPush stops at command %08X, %u bytes before the end", head, end - p);
            break;
        }
        for (uint32_t i = 0; i < n; i++, p += 4) {
            uint32_t m = head & 0x40000000 ? method : method + i * 4, v = R32(p);
            ApplyVisibilityMethod(m, v);
            if (m == kInlineArray) {
                if (prim) vertices.push_back(v);
            } else if (m != kBeginEnd) {
                if (m >> 2 < g_state.simple.size()) g_state.simple[m >> 2] = v;
            } else if (v) {
                prim = v;
                vertices.clear();
            } else if (prim) {
                uint32_t stride = VertexBytes(), bytes = uint32_t(vertices.size()) * 4;
                if (stride && bytes % stride == 0) DrawUp(prim, bytes / stride, vertices.data(), stride);
                else LOG_FIRST(8, "d3d: EndPush primitive %u dropped, %u bytes of vertices of %u", prim, bytes, stride);
                prim = 0;
            }
        }
    }
    g_push_begin = 0;
    ScratchPush();
    return a[0];
}

// RunPushBuffer(push buffer, fixups): a prebuilt NV2A command stream (models built offline, or recorded) run as the
// GPU would run it. Its methods go to the state as they come; vertex programs and constants load as on the GPU, and at
// each end of a primitive the arrays, textures and combiners the methods set are turned into a draw.
namespace {
namespace pb {
uint32_t g_prog[136 * 4], g_prog_load = 0, g_prog_comp = 0, g_const_load = 0, g_const_comp = 0;
uint32_t g_vb_headers = 0, g_tex_headers = 0, g_pal_headers = 0;  // 16 vertex buffer and 4 texture headers the draws point at
uint32_t g_prim = 0;
bool g_in_run = false;  // inside RunPushBuffer, not the title's own pushes
bool g_prog_loaded = false;  // a vertex program came in: the stream draws with it
bool g_fog_seen = false;     // SET_FOG_ENABLE came through a push: the fog methods hold the hardware's fog
std::vector<uint32_t> g_indices;
std::vector<uint32_t> g_inline_pb;  // an inline array's dwords between begin and end

uint64_t Fnv(const void* p, size_t n, uint64_t h) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

// an address the GPU reads: physical, or one of its CPU views
uint32_t Guest(uint32_t a) { return mem::VirtualFromPhysical(a >= 0x80000000 ? a & 0x0FFFFFFF : a); }

uint32_t Simple(uint32_t m) { return g_state.simple[m >> 2]; }

// NV2A vertex array format (type in bits 0-3, components in 4-7) as a D3DVSDT
uint32_t VsdType(uint32_t f) {
    uint32_t type = f & 0xF, size = (f >> 4) & 0xF;
    switch (type) {
        case 0: return size == 4 ? 0x40 : (size << 4) | 0x4;  // D3DCOLOR, else bytes
        case 1: return (size << 4) | 0x1;                       // NORMSHORT
        case 2: return (size << 4) | 0x2;                       // FLOAT
        case 4: return (size << 4) | 0x4;                       // PBYTE
        case 5: return (size << 4) | 0x5;                       // SHORT
        case 6: return 0x16;                                    // NORMPACKED3
        default: return (size << 4) | 0x2;
    }
}

uint32_t FormatBytes(uint32_t f) {
    uint32_t type = f & 0xF, size = (f >> 4) & 0xF;
    static const uint8_t kComponent[8] = {1, 2, 4, 0, 1, 2, 4, 0};  // UB_D3D S1 F - UB_OGL S32K CMP -
    return type == 6 ? 4 : size * kComponent[type & 7];
}

std::shared_ptr<gpu::VertexShader> InlineLayout(const std::shared_ptr<gpu::VertexShader>& program, uint32_t& stride) {
    static std::map<std::pair<const gpu::VertexShader*, uint64_t>, std::shared_ptr<gpu::VertexShader>> made;
    uint32_t formats[16];
    stride = 0;
    for (int i = 0; i < 16; i++) {
        formats[i] = Simple(0x1760 + i * 4);
        if ((formats[i] >> 4) & 0xF) stride += FormatBytes(formats[i]);
    }
    if (!stride) return nullptr;
    auto& vs = made[{program.get(), Fnv(formats, sizeof(formats), 1469598103934665603ull)}];
    if (!vs) {
        vs = std::make_shared<gpu::VertexShader>();
        if (program) {
            vs->program = program->program;
            vs->program_type = program->program_type;
            vs->handle = program->handle;  // the backend keeps its cached translation by object
        } else {
            vs->handle = 0x48000001u + uint32_t(made.size()) * 2;
        }
        vs->handle |= 1;
        uint32_t off = 0;
        for (int i = 0; i < 16; i++) {
            if (!((formats[i] >> 4) & 0xF)) continue;
            vs->inputs[size_t(i)] = {0, off, VsdType(formats[i])};
            off += FormatBytes(formats[i]);
        }
        vs->stream_stride[0] = off;
    }
    return vs;
}

void LoadProgram(const std::vector<uint32_t>& prog, uint32_t address) {
    for (size_t i = 0; i < prog.size() && address * 4 + i < std::size(g_prog); i++) g_prog[address * 4 + i] = prog[i];
    g_prog_loaded = true;
}

std::vector<uint32_t> ProgramFrom(uint32_t start) {
    std::vector<uint32_t> prog;
    for (uint32_t i = start; i < 136; i++) {
        prog.insert(prog.end(), g_prog + i * 4, g_prog + i * 4 + 4);
        if (g_prog[i * 4 + 3] & 1) break;  // the last instruction
    }
    return prog;
}

std::shared_ptr<gpu::VertexShader> Program() {
    static std::unordered_map<uint64_t, std::shared_ptr<gpu::VertexShader>> made;
    std::vector<uint32_t> prog;
    for (uint32_t i = Simple(0x1EA0) & 0xFF; i < 136; i++) {
        prog.insert(prog.end(), g_prog + i * 4, g_prog + i * 4 + 4);
        if (g_prog[i * 4 + 3] & 1) break;  // the last instruction
    }
    uint32_t formats[16];
    for (int i = 0; i < 16; i++) formats[i] = Simple(0x1760 + i * 4);
    uint64_t key = Fnv(formats, sizeof(formats), Fnv(prog.data(), prog.size() * 4, 1469598103934665603ull));
    auto& vs = made[key];
    if (!vs) {
        vs = std::make_shared<gpu::VertexShader>();
        vs->handle = 0x40000001u + uint32_t(made.size()) * 2;
        vs->program = std::move(prog);
        vs->program_type = 0x2078;
        for (int i = 0; i < 16; i++) {
            if (!((formats[i] >> 4) & 0xF)) continue;
            vs->inputs[size_t(i)] = {i, 0, VsdType(formats[i])};
            vs->stream_stride[size_t(i)] = formats[i] >> 8;
        }
    }
    return vs;
}

std::shared_ptr<gpu::PixelShader> Combiners() {
    static std::unordered_map<uint64_t, std::shared_ptr<gpu::PixelShader>> made;
    uint32_t d[0xF0 / 4] = {};
    for (int i = 0; i < 8; i++) {
        d[i] = Simple(0x260 + i * 4);
        d[10 + i] = Simple(0xA60 + i * 4);
        d[18 + i] = Simple(0xA80 + i * 4);
        d[26 + i] = Simple(0xAA0 + i * 4);
        d[34 + i] = Simple(0xAC0 + i * 4);
        d[45 + i] = Simple(0x1E40 + i * 4);
    }
    d[8] = Simple(0x288);
    d[9] = Simple(0x28C);
    d[42] = Simple(0x17F8);
    d[43] = Simple(0x1E20);
    d[44] = Simple(0x1E24);
    d[53] = Simple(0x1E60);
    d[54] = Simple(0x1E70);  // SET_SHADER_STAGE_PROGRAM
    d[55] = Simple(0x1E74);
    d[56] = Simple(0x1E78);
    auto& ps = made[Fnv(d, sizeof(d), 1469598103934665603ull)];
    if (!ps) {
        ps = std::make_shared<gpu::PixelShader>();
        ps->handle = 0x50000000u + uint32_t(made.size());
        std::memcpy(ps->def, d, sizeof(d));
    }
    return ps;
}

// the arrays, textures, combiners and raster states the methods set, into the state a draw reads
void Bind() {
    if (!g_vb_headers) {
        g_vb_headers = mem::AllocSystem(16 * 16);
        g_tex_headers = mem::AllocSystem(4 * 32);
        g_pal_headers = mem::AllocSystem(4 * 16);
    }
    g_state.vs = Program();
    g_state.vs_handle = g_state.vs->handle;
    for (uint32_t i = 0; i < 16; i++) {
        uint32_t f = Simple(0x1760 + i * 4), h = g_vb_headers + i * 16;
        W32(h, 0x00000001);  // vertex buffer, one reference
        W32(h + 4, Simple(0x1720 + i * 4) & 0x0FFFFFFF);
        W32(h + 8, 0);
        // an array the stream never set points nowhere: past physical memory it is left out
        bool real = (f >> 4) & 0xF && (Simple(0x1720 + i * 4) & 0x0FFFFFFF) < mem::kPhysicalSize;
        g_state.streams[i] = {real ? h : 0u, f >> 8};
    }
    for (uint32_t s = 0; s < 4; s++) {
        uint32_t base = 0x1B00 + s * 0x40, c0 = Simple(base + 0xC);
        g_state.palettes[s] = 0;
        if (!(c0 & 0x40000000) || (Simple(base) & 0x0FFFFFFF) >= mem::kPhysicalSize) {
            g_state.textures[s] = 0;
            continue;
        }
        uint32_t h = g_tex_headers + s * 32, rect = Simple(base + 0x1C), pitch = Simple(base + 0x10) >> 16;
        uint32_t w = rect >> 16, ht = rect & 0xFFFF;
        W32(h, 0x00040001);  // texture, one reference
        W32(h + 4, Simple(base) & 0x0FFFFFFF);
        W32(h + 8, 0);
        // a swizzled texture's size is in its format (log2 dimensions), only a linear one has a size field. The format
        // code decides: a swizzled or compressed one without its log2 sizes takes them from the image rect
        uint32_t format = Simple(base + 4);
        const gpu::FormatInfo& fi = gpu::Format((format >> 8) & 0xFF);
        bool swizzled = fi.swizzled || fi.compressed || ((format >> 20) & 0xFF);
        if ((fi.swizzled || fi.compressed) && !((format >> 20) & 0xFF) && w && ht) {
            auto log2 = [](uint32_t v) { uint32_t l = 0; while ((1u << (l + 1)) <= v && l < 15) l++; return l; };
            format |= log2(w) << 20 | log2(ht) << 24;
            LOG_FIRST(8, "d3d: push texture format %08X (%s) without its log2 sizes, %ux%u from the image rect", Simple(base + 4),
                      fi.name, w, ht);
        }
        W32(h + 12, format);
        W32(h + 16, !swizzled && w && ht ? (w - 1) | (ht - 1) << 12 | (pitch >= 64 ? pitch / 64 - 1 : 0) << 24 : 0);
        g_state.textures[s] = h;
        // SET_TEXTURE_PALETTE: address (64 byte aligned), size code in bits 2-3 (256, 128, 64, 32 colours)
        uint32_t pal = Simple(base + 0x20);
        if (pal & ~0x3Fu) {
            uint32_t ph = g_pal_headers + s * 16;
            W32(ph, 0x00030001 | ((pal >> 2) & 3) << 30);
            W32(ph + 4, pal & 0x0FFFFFC0);
            W32(ph + 8, 0);
            g_state.palettes[s] = ph;
        }
        auto& t = g_state.tss[s];
        uint32_t addr = Simple(base + 8), filt = Simple(base + 0x14);
        t[gpu::kTssAddressU] = addr & 0xF;
        t[gpu::kTssAddressV] = (addr >> 8) & 0xF;
        t[gpu::kTssAddressW] = (addr >> 16) & 0xF;
        static const uint8_t kMin[8][2] = {{1, 0}, {1, 0}, {2, 0}, {1, 1}, {2, 1}, {1, 2}, {2, 2}, {2, 0}};
        uint32_t min = (filt >> 16) & 7, mag = (filt >> 24) & 7;
        t[gpu::kTssMinFilter] = kMin[min][0];
        t[gpu::kTssMipFilter] = kMin[min][1];
        t[gpu::kTssMagFilter] = mag == 1 ? 1 : 2;
        t[gpu::kTssAlphaKill] = c0 & 4 ? 4 : 0;
        t[gpu::kTssBorderColor] = Simple(base + 0x24);
        // SET_TEXTURE_SET_BUMP_ENV_MAT, _SCALE, _OFFSET: the D3DTSS order from BUMPENVMAT00 (0x537AD0 pushes it so)
        for (uint32_t k = 0; k < 6; k++) t[gpu::kTssBumpEnvMat00 + k] = Simple(base + 0x28 + k * 4);
    }
    g_state.ps = Combiners();
    g_state.rs[gpu::kRsPsTextureModes] = Simple(0x1E70);
    // the complex states the renderer keeps as D3D values
    g_state.rs[gpu::kRsZEnable] = Simple(0x30C);
    g_state.rs[gpu::kRsStencilEnable] = Simple(0x32C);
    g_state.rs[gpu::kRsStencilFail] = Simple(0x370);
    g_state.rs[gpu::kRsFillMode] = Simple(0x38C) ? Simple(0x38C) : 0x1B02;
    // the XDK sets the hardware front face to the opposite of D3DRS_FRONTFACE (screen y runs down), and the cull face to
    // front when D3DRS_CULLMODE names the D3D front face's winding: the culled winding is read back the same way
    uint32_t cull = 0;
    if (Simple(0x308)) {
        uint32_t d3d_front = (Simple(0x3A0) == 0x900 ? 0x900 : 0x901) ^ 1;
        cull = Simple(0x39C) == 0x404 ? d3d_front : d3d_front ^ 1;  // D3DCULL_CW 0x900 / D3DCULL_CCW 0x901
    }
    g_state.rs[gpu::kRsCullMode] = cull;
    // fog as LazySetStates or the recorded buffer last left it: D3DRS_FOG* may have moved on since. The XDK writes
    // linear as p0 + d p1 - 1 (p0 = 1 + end / (end - start), p1 = -1 / (end - start); 1 and 1 for table none,
    // the vertex program's oFog as is), exp as p1 = density * -0.0901681 and exp2 as p1 = density * -0.21233
    if (g_fog_seen) {
        auto fbits = [](float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; };
        auto fval = [](uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; };
        uint32_t mode = Simple(0x29C);
        float p0 = fval(Simple(0x9C0)), p1 = fval(Simple(0x9C4));
        uint32_t table = 3;  // D3DFOG_LINEAR
        float start = 1, end = 0, density = 0;
        if (mode == 0x800 || mode == 0x802) {
            table = 1, density = p1 / -0.0901680737733841f;
        } else if (mode == 0x801 || mode == 0x803) {
            table = 2, density = std::fabs(p1 / 0.21233002841472626f);
        } else if (p1 != 0) {
            start = (2 - p0) / p1, end = (1 - p0) / p1;
        }
        if (g_state.rs[gpu::kRsFogEnable] != (Simple(0x2A4) ? 1u : 0u))
            LOG_FIRST(20, "d3d: push fog enable %u mode %X %g %g, D3D states had enable %u table %u", Simple(0x2A4), mode, p0, p1,
                      g_state.rs[gpu::kRsFogEnable], g_state.rs[gpu::kRsFogTableMode]);
        g_state.rs[gpu::kRsFogEnable] = Simple(0x2A4) ? 1 : 0;
        g_state.rs[gpu::kRsFogTableMode] = table;
        g_state.rs[gpu::kRsFogStart] = fbits(start);
        g_state.rs[gpu::kRsFogEnd] = fbits(end);
        g_state.rs[gpu::kRsFogDensity] = fbits(density);
    }
    static uint64_t seen = 0;
    uint64_t key = uint64_t(Simple(0x308)) << 40 ^ uint64_t(Simple(0x39C)) << 20 ^ Simple(0x3A0);
    if (key != seen && (seen = key, true))
        LOG_FIRST(20, "d3d: push cull enable %X face %X front %X -> %X", Simple(0x308), Simple(0x39C), Simple(0x3A0), cull);
}

void EndPrimitiveBound();

// a draw sets up its own program, arrays, textures and combiners: what the title had bound comes back after it. So do
// the texture modes: the library sends them again from the pixel shader's definition (device +0x790, 0x541C50) on its
// next texture or shader change, where the cull, depth and stencil registers the buffer wrote stay as the console's do
void EndPrimitive() {
    uint32_t tex_modes = g_state.rs[gpu::kRsPsTextureModes];
    auto vs = g_state.vs;
    auto ps = g_state.ps;
    uint32_t vs_handle = g_state.vs_handle;
    auto textures = g_state.textures;
    auto palettes = g_state.palettes;
    auto streams = g_state.streams;
    auto tss = g_state.tss;
    EndPrimitiveBound();
    g_state.rs[gpu::kRsPsTextureModes] = tex_modes;
    g_state.vs = vs;
    g_state.ps = ps;
    g_state.vs_handle = vs_handle;
    g_state.textures = textures;
    g_state.palettes = palettes;
    g_state.streams = streams;
    g_state.tss = tss;
}

void EndPrimitiveBound() {
    if (g_prim && !g_inline_pb.empty() && g_prog_loaded && g_backend) {
        SyncDeferred();
        Bind();
        uint32_t stride = 0;
        if (auto layout = InlineLayout(g_state.vs, stride); layout && stride && (g_inline_pb.size() * 4) % stride == 0) {
            g_state.vs = layout;
            DrawUp(g_prim, uint32_t(g_inline_pb.size() * 4 / stride), g_inline_pb.data(), stride);
        }
    }
    g_inline_pb.clear();
    if (!g_prim || g_indices.empty()) {
        g_prim = 0;
        g_indices.clear();
        return;
    }
    if (!g_prog_loaded) {
        LOG_FIRST(4, "d3d: push buffer draw without a vertex program, skipped");
    } else if (g_backend) {
        SyncDeferred();
        Bind();
        LOG_FIRST(30, "d3d: array draw prim %u, %zu indices, start %u, arrays %08X/%08X %08X/%08X %08X/%08X", g_prim, g_indices.size(),
                  Simple(0x1EA0), Simple(0x1720), Simple(0x1760), Simple(0x1724), Simple(0x1764), Simple(0x1728), Simple(0x1768));
        if (g_state.textures[0] | g_state.textures[1]) LOG_FIRST(20, "d3d: push buffer draw: stage modes %08X, textures %08X %08X %08X %08X, control0 %08X %08X", Simple(0x1E70),
                  g_state.textures[0], g_state.textures[1], g_state.textures[2], g_state.textures[3], Simple(0x1B0C), Simple(0x1B4C));
        uint32_t lo = *std::min_element(g_indices.begin(), g_indices.end());
        uint32_t hi = *std::max_element(g_indices.begin(), g_indices.end());
        // every array must hold the vertices the indices reach, or the stream is not what the draw thinks
        bool inside = true;
        for (uint32_t i = 0; i < 16; i++) {
            uint32_t f = Simple(0x1760 + i * 4);
            if (!g_state.streams[i].vb) continue;
            uint64_t end = uint64_t(Simple(0x1720 + i * 4) & 0x0FFFFFFF) + uint64_t(hi + 1) * (f >> 8);
            if (end > mem::kPhysicalSize) inside = false;
        }
        static uint32_t last_zs = ~0u;
        if (g_state.zs.data != last_zs && (last_zs = g_state.zs.data, true)) LOG_FIRST(40, "d3d: push draw depth surface %08X format %08X zscale %g c-38 %g %g %g", g_state.zs.data, g_state.zs.format,
                  g_state.zscale, g_state.vsc[58][0], g_state.vsc[58][1], g_state.vsc[58][2]);
        // XBE_PBLOG_AFTER=<s>: the push buffer draw logs below start that many seconds into the run
        static const auto pb_log_start = std::chrono::steady_clock::now();
        static const double pb_log_after = std::getenv("XBE_PBLOG_AFTER") ? std::atof(std::getenv("XBE_PBLOG_AFTER")) : 0;
        bool pb_log = std::chrono::duration<double>(std::chrono::steady_clock::now() - pb_log_start).count() >= pb_log_after;
        if (pb_log && g_indices.size() > 300) {
            std::string idx;
            for (size_t i = 0; i < 24 && i < g_indices.size(); i++) idx += " " + std::to_string(g_indices[i]);
            LOG_FIRST(400, "d3d: big push draw (%s) prim %u, %zu indices %u..%u, c0 %g %g %g %g:%s", g_in_run ? "run" : "walk", g_prim,
                      g_indices.size(), lo, hi, g_state.vsc[96][0], g_state.vsc[96][1], g_state.vsc[96][2], g_state.vsc[96][3], idx.c_str());
            for (uint32_t i = 0; i < 16; i++)
                if ((Simple(0x1760 + i * 4) >> 4) & 0xF)
                    LOG_FIRST(400, "d3d:   array %u at %08X format %08X", i, Simple(0x1720 + i * 4), Simple(0x1760 + i * 4));
        }
        if (!inside) {
            LOG_FIRST(8, "d3d: push buffer draw past physical memory skipped (vertices %u..%u)", lo, hi);
        } else if (hi - lo < 0x10000) {
            static std::vector<uint16_t> idx;
            idx.resize(g_indices.size());
            for (size_t i = 0; i < g_indices.size(); i++) idx[i] = uint16_t(g_indices[i] - lo);
            // the vertices the draw reads, copied now: the title rewrites dynamic arrays (shadows, effects) once its
            // own resources look idle, which these headers of ours never tell it, while the renderer reads them later
            bool copied = true;
            for (uint32_t i = 0; i < 16; i++) {
                const auto& st = g_state.streams[i];
                if (!st.vb || !st.stride) continue;
                uint64_t bytes = uint64_t(hi - lo + 1) * st.stride;
                if (bytes > kUpHalf) {
                    copied = false;
                    break;
                }
                uint32_t src = Guest(Simple(0x1720 + i * 4) & 0x0FFFFFFF) + lo * st.stride;
                uint32_t up = UpVertices(HostPtr(src), uint32_t(bytes));
                W32(st.vb + 4, R32(up + 4));
            }
            uint32_t base = g_state.base_vertex;
            g_state.base_vertex = copied ? 0 : lo;
            if (!copied)  // too many to copy: read in place, as before
                for (uint32_t i = 0; i < 16; i++)
                    if (g_state.streams[i].vb) W32(g_state.streams[i].vb + 4, Simple(0x1720 + i * 4) & 0x0FFFFFFF);
            g_backend->Draw(g_state, g_prim, 0, uint32_t(idx.size()), reinterpret_cast<const uint8_t*>(idx.data()));
            g_state.base_vertex = base;
        } else {
            LOG_FIRST(4, "d3d: push buffer draw over %u vertices skipped", hi - lo);
        }
    }
    g_prim = 0;
    g_indices.clear();
}

void Method(uint32_t m, uint32_t v) {
    if (m >= 0x1B00 && m < 0x1C00 && (m & 0x3F) == 0 && Watched(v)) LOG_FIRST(40, "watch: push texture %u offset %08X (%s)", (m - 0x1B00) / 0x40, v, g_in_run ? "run" : "walk");
    float f;
    std::memcpy(&f, &v, 4);
    if (m == 0x17FC) {  // begin/end
        if (v) {
            g_prim = v;
            g_indices.clear();
            g_inline_pb.clear();
        } else {
            EndPrimitive();
        }
    } else if (m == 0x1800) {  // two 16 bit indices
        g_indices.push_back(v & 0xFFFF);
        g_indices.push_back(v >> 16);
    } else if (m == 0x1808) {
        g_indices.push_back(v);
    } else if (m == 0x1810) {  // first vertex, count - 1 in the top byte
        for (uint32_t i = 0, first = v & 0xFFFFFF, n = (v >> 24) + 1; i < n; i++) g_indices.push_back(first + i);
    } else if (m == 0x1818) {
        g_inline_pb.push_back(v);
    } else if (m >= 0x0B00 && m < 0x0B80) {  // vertex program, 4 dwords an instruction
        if (g_prog_load < 136) g_prog[g_prog_load * 4 + g_prog_comp] = v;
        g_prog_loaded = true;
        if (++g_prog_comp == 4) g_prog_comp = 0, g_prog_load++;
    } else if (m >= 0x0B80 && m < 0x0C00) {  // constants, 4 floats a register
        if (g_const_load < 192) g_state.vsc[g_const_load][g_const_comp] = f;
        if (++g_const_comp == 4) g_const_comp = 0, g_const_load++;
    } else {
        if (m == 0x1E9C) g_prog_load = v, g_prog_comp = 0;
        if (m == 0x1EA4) g_const_load = v, g_const_comp = 0;
        if (m == 0x1EA4 || m == 0x1EA0) {
            static const auto t0 = std::chrono::steady_clock::now();
            static const double after = std::getenv("XBE_PBLOG_AFTER") ? std::atof(std::getenv("XBE_PBLOG_AFTER")) : 1e9;
            static int n = 0;
            if (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() >= after && n++ < 200)
                LOGI("d3d: %s %u (%s)", m == 0x1EA4 ? "const load" : "prog start", v, g_in_run ? "run" : "walk");
        }
        if (m == 0x2A4) g_fog_seen = true;
        // surface and clear methods inside a recorded buffer are not followed: worth knowing if a title uses them
        if (g_in_run && ((m >= 0x200 && m <= 0x214) || (m >= 0x1D8C && m <= 0x1D94)))
            LOG_FIRST(16, "d3d: push buffer sets method %X = %08X (surface or clear, not followed)", m, v);
        ApplyVisibilityMethod(m, v);
        g_state.simple[m >> 2] = v;
    }
}

// from p until the stream comes back to end, following the jumps, calls and returns the fixups and the buffer hold
void Run(uint32_t p, uint32_t end) {
    uint32_t ret = 0;
    for (int guard = 1 << 22; guard > 0 && p != end; guard--) {
        uint32_t head = R32(p);
        p += 4;
        if ((head & 3) == 1) {  // jump, old form: address | 1
            static int outside = 0;
            uint32_t to = Guest(head & ~3u);
            if ((to < end - 0x100000 || to > end) && outside++ < 40)
                LOGI("d3d: push buffer jump out %08X -> %08X (end %08X) %08X %08X %08X", p - 4, to, end, R32(to), R32(to + 4), R32(to + 8));
            p = Guest(head & ~3u);
            continue;
        }
        if ((head & 0xE0000003) == 0x20000000) {  // jump
            p = Guest(head & 0x1FFFFFFC);
            continue;
        }
        if ((head & 3) == 2) {  // call
            LOG_FIRST(30, "d3d: push buffer call %08X -> %08X", p - 4, Guest(head & ~3u));
            ret = p;
            p = Guest(head & ~3u);
            continue;
        }
        if (head == 0x00020000) {  // return
            if (!ret) break;
            p = ret;
            ret = 0;
            continue;
        }
        if (head & 0xA0030003) {
            LOG_FIRST(8, "d3d: push buffer stops at command %08X", head);
            break;
        }
        uint32_t method = head & 0x1FFC, n = (head >> 18) & 0x7FF;
        bool same = head & 0x40000000;
        for (uint32_t i = 0; i < n; i++, p += 4) Method(same ? method : method + i * 4, R32(p));
    }
}

// fixups: size, offset into the buffer, then size bytes copied there, until a size of -1
void ApplyFixups(uint32_t fixup, uint32_t buffer) {
    uint32_t data = Guest(R32(fixup + 4)), p = data + R32(fixup + 12), end = data + R32(fixup + 16);
    static int logged = 0;
    while (p + 8 <= end) {
        uint32_t size = R32(p), offset = R32(p + 4);
        if (size == 0xFFFFFFFF) break;
        static const auto t0 = std::chrono::steady_clock::now();
        static const double after = std::getenv("XBE_PBLOG_AFTER") ? std::atof(std::getenv("XBE_PBLOG_AFTER")) : 0;
        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() >= after && logged++ < 120)
            LOGI("d3d: fixup size %X at %X of %08X: %08X %08X %08X", size, offset, buffer, R32(p + 8), size > 4 ? R32(p + 12) : 0,
                 size > 8 ? R32(p + 16) : 0);
        std::memcpy(HostPtr(buffer + offset), HostPtr(p + 8), size);
        p += 8 + ((size + 3) & ~3u);
    }
}
}  // namespace pb
}  // namespace

HLE_STDCALL(D3DDevice_RunPushBuffer, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    uint32_t buf = a[0], fixup = a[1];
    if (!buf) return 0;
    // as the library does first: the states left pending (fog, ...) reach the hardware before the buffer, and the
    // vertex formats the buffer overwrites are sent again by the next draw
    // with the shader dirty it reads the device's current shader object, which SelectVertexShader(0) leaves null
    uint32_t shader = Dev(XDK_CDevice_VertexShader);
#if HAS_D3D_FvfShader
    if (!shader) SetDev(XDK_CDevice_VertexShader, SYM(D3D_FvfShader));
#endif
    // XBE_NOPBSTATES: skipped, as before, to compare
    static const bool skip = std::getenv("XBE_NOPBSTATES") != nullptr;
    if (!skip && Dev(XDK_CDevice_VertexShader)) kernel::CallGuest(ctx, kSetStateFlags, {R32(kDevicePtr), 0});
    SetDev(XDK_CDevice_VertexShader, shader);
    Walk(Dev(kDevPush));
    uint32_t data = pb::Guest(R32(buf + 4)), size = R32(buf + 12);
    if (fixup) pb::ApplyFixups(fixup, data);
    // what the title had bound stays bound for its own draws after this
    auto vs = g_state.vs;
    auto ps = g_state.ps;
    uint32_t vs_handle = g_state.vs_handle;
    auto textures = g_state.textures;
    auto palettes = g_state.palettes;
    auto streams = g_state.streams;
    auto tss = g_state.tss;
    pb::g_in_run = true;
    pb::Run(data, data + size);
    pb::g_in_run = false;
    g_state.vs = vs;
    g_state.ps = ps;
    g_state.vs_handle = vs_handle;
    g_state.textures = textures;
    g_state.palettes = palettes;
    g_state.streams = streams;
    g_state.tss = tss;
    return 0;
}

// frame buffer compression tiles are the console's memory controller's business
HLE_STDCALL(D3DDevice_SetTile, 2) { return 0; }

// the library's Begin pushes the start of the primitive, here it is opened
HLE_STDCALL(D3DDevice_Begin, 1) {
    XDK_REQUIRE("d3d", XDK_KEY(CDevice_Flags));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    g_im_prim = a[0];
    g_im_mask = 0;
    g_im_vertices.clear();
    SetDev(XDK_CDevice_Flags, Dev(XDK_CDevice_Flags) | 0x800);
    return 0;
}
// the library's End pushes the end of the primitive, here it is drawn
HLE_STDCALL(D3DDevice_End, 0) {
    XDK_REQUIRE("d3d", XDK_KEY(CDevice_Flags));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    EndPrimitive();
    SetDev(XDK_CDevice_Flags, Dev(XDK_CDevice_Flags) & ~0x1800u);
    return 0;
}
