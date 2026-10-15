// MinHook-based inline hooks on the D3D11 runtime's context and device methods.
//
// Method bodies are read from the vtables of live objects: the compositor's own device as soon as
// it is created, a probe device, and every device / context the compositor touches later. One
// D3D11 runtime can route the same method through different bodies for different contexts (for
// example a locking variant once multithread protection is switched on, or a deferred context),
// so every distinct body found is hooked, each with its own detour instance (template index) and
// trampoline. A per-thread guard makes nested calls (one variant forwarding to another, or our own
// D3D calls made from inside a detour) pass straight through.
#include "common.h"
#include "vtables.h"

#include <d3d11_4.h>
#include <d3d10.h>

#include "MinHook.h"

namespace vu {

PFN_SetSRVs g_origPSSetShaderResources = nullptr;
PFN_SetSRVs g_origCSSetShaderResources = nullptr;
volatile LONG g_shuttingDown = 0;

namespace {

// ---------------------------------------------------------------------------------------------
// Per-thread "inside vrupscale" guard

DWORD g_tls = TLS_OUT_OF_INDEXES;

inline bool InVu()
{
    if (g_tls == TLS_OUT_OF_INDEXES) return false;
    DWORD e = GetLastError();   // TlsGetValue resets the last error; keep the caller's
    bool in = TlsGetValue(g_tls) != nullptr;
    SetLastError(e);
    return in;
}

// CPU time the compositor's threads spend in vrupscale's handlers (not in the calls passed on)
struct HookCpu {
    LARGE_INTEGER t0;
    HookCpu() { QueryPerformanceCounter(&t0); }
    ~HookCpu()
    {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        NoteHookCpu(t.QuadPart - t0.QuadPart);
    }
};

struct VuScope {
    bool outer;
    VuScope() : outer(g_tls != TLS_OUT_OF_INDEXES && !InVu())
    {
        if (outer) TlsSetValue(g_tls, (void *)1);
    }
    ~VuScope()
    {
        if (outer) TlsSetValue(g_tls, nullptr);
    }
};

// ---------------------------------------------------------------------------------------------
// Context methods, each with up to kVariants hooked bodies

constexpr int kVariants = 6;

typedef void (STDMETHODCALLTYPE *PFN_Draw)(ID3D11DeviceContext *, UINT, UINT);
typedef void (STDMETHODCALLTYPE *PFN_DrawIndexed)(ID3D11DeviceContext *, UINT, UINT, INT);
typedef void (STDMETHODCALLTYPE *PFN_DrawInstanced)(ID3D11DeviceContext *, UINT, UINT, UINT, UINT);
typedef void (STDMETHODCALLTYPE *PFN_DrawIndexedInstanced)(ID3D11DeviceContext *, UINT, UINT, UINT, INT, UINT);
typedef void (STDMETHODCALLTYPE *PFN_DrawAuto)(ID3D11DeviceContext *);
typedef void (STDMETHODCALLTYPE *PFN_DrawIndirect)(ID3D11DeviceContext *, ID3D11Buffer *, UINT);
typedef void (STDMETHODCALLTYPE *PFN_ClearState)(ID3D11DeviceContext *);
typedef void (STDMETHODCALLTYPE *PFN_CopyResource)(ID3D11DeviceContext *, ID3D11Resource *, ID3D11Resource *);
typedef void (STDMETHODCALLTYPE *PFN_CopySubresourceRegion)(ID3D11DeviceContext *, ID3D11Resource *, UINT, UINT, UINT, UINT,
                                                            ID3D11Resource *, UINT, const D3D11_BOX *);
typedef void (STDMETHODCALLTYPE *PFN_CopySubresourceRegion1)(ID3D11DeviceContext1 *, ID3D11Resource *, UINT, UINT, UINT, UINT,
                                                             ID3D11Resource *, UINT, const D3D11_BOX *, UINT);
typedef HRESULT (STDMETHODCALLTYPE *PFN_FinishCommandList)(ID3D11DeviceContext *, BOOL, ID3D11CommandList **);
typedef void (STDMETHODCALLTYPE *PFN_ExecuteCommandList)(ID3D11DeviceContext *, ID3D11CommandList *, BOOL);
typedef HRESULT (STDMETHODCALLTYPE *PFN_OpenSharedResource)(ID3D11Device *, HANDLE, REFIID, void **);
typedef HRESULT (STDMETHODCALLTYPE *PFN_OpenSharedResource1)(ID3D11Device1 *, HANDLE, REFIID, void **);
typedef HRESULT (STDMETHODCALLTYPE *PFN_OpenSharedResourceByName)(ID3D11Device1 *, LPCWSTR, DWORD, REFIID, void **);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateDeferredContext)(ID3D11Device *, UINT, ID3D11DeviceContext **);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateDeferredContext1)(ID3D11Device1 *, UINT, ID3D11DeviceContext1 **);
typedef BOOL (STDMETHODCALLTYPE *PFN_SetMT)(IUnknown *, BOOL);   // ID3D11Multithread or ID3D10Multithread
typedef HRESULT (WINAPI *PFN_D3D11CreateDevice)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *,
                                                UINT, UINT, ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);

PFN_SetSRVs g_oPS[kVariants];
PFN_SetSRVs g_oCS[kVariants];
PFN_Draw g_oDraw[kVariants];
PFN_DrawIndexed g_oDrawIndexed[kVariants];
PFN_DrawInstanced g_oDrawInstanced[kVariants];
PFN_DrawIndexedInstanced g_oDrawIndexedInstanced[kVariants];
PFN_DrawAuto g_oDrawAuto[kVariants];
PFN_DrawIndirect g_oDrawInstancedIndirect[kVariants];
PFN_DrawIndirect g_oDrawIndexedInstancedIndirect[kVariants];
PFN_ClearState g_oClearState[kVariants];
PFN_CopyResource g_oCopyResource[kVariants];
PFN_CopySubresourceRegion g_oCopySubresourceRegion[kVariants];
PFN_CopySubresourceRegion1 g_oCopySubresourceRegion1[kVariants];
PFN_FinishCommandList g_oFinishCommandList[kVariants];
PFN_ExecuteCommandList g_oExecuteCommandList[kVariants];

PFN_OpenSharedResource g_origOpenSharedResource;
PFN_OpenSharedResource1 g_origOpenSharedResource1;
PFN_OpenSharedResourceByName g_origOpenSharedResourceByName;
PFN_CreateDeferredContext g_origCreateDeferredContext;
PFN_CreateDeferredContext1 g_origCreateDeferredContext1;
PFN_SetMT g_origSetMT[2];
LONG g_psHits[kVariants], g_drawHits[kVariants];   // which hooked bodies the calls arrive through

volatile LONG g_installed = 0;

// ---- detours (V = which hooked body this instance stands in for) ----------------------------

template <int V>
void STDMETHODCALLTYPE Hook_PSSetShaderResources(ID3D11DeviceContext *ctx, UINT start, UINT num,
                                                 ID3D11ShaderResourceView *const *views)
{
    if (g_shuttingDown || InVu()) return g_oPS[V](ctx, start, num, views);
    VuScope scope;
    InterlockedIncrement(&g_psHits[V]);
    ID3D11ShaderResourceView *local[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT];
    bool replaced = false;
    if (views && num > 0 && start < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT &&
        num <= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - start) {
        HookCpu cpu;
        OnPSSetShaderResources(ctx, start, num, views, local, &replaced);
    }
    g_oPS[V](ctx, start, num, replaced ? local : views);
}

template <int V>
void STDMETHODCALLTYPE Hook_CSSetShaderResources(ID3D11DeviceContext *ctx, UINT start, UINT num,
                                                 ID3D11ShaderResourceView *const *views)
{
    if (g_shuttingDown || InVu()) return g_oCS[V](ctx, start, num, views);
    VuScope scope;
    InterlockedIncrement(&g_stats.csHookCalls);
    if (views && num > 0 && num <= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT) OnCSSetShaderResources(ctx, num, views);
    g_oCS[V](ctx, start, num, views);
}

#define VU_DRAW_HOOK(NAME, ARR, PARAMS, ARGS)                       \
    template <int V> void STDMETHODCALLTYPE NAME PARAMS             \
    {                                                               \
        if (g_shuttingDown || InVu()) return ARR[V] ARGS;           \
        VuScope scope;                                              \
        InterlockedIncrement(&g_stats.drawHookCalls);               \
        InterlockedIncrement(&g_drawHits[V]);                       \
        { HookCpu cpu; OnBeforeDraw(ctx); }                         \
        ARR[V] ARGS;                                                \
    }

VU_DRAW_HOOK(Hook_Draw, g_oDraw, (ID3D11DeviceContext *ctx, UINT a, UINT b), (ctx, a, b))
VU_DRAW_HOOK(Hook_DrawIndexed, g_oDrawIndexed, (ID3D11DeviceContext *ctx, UINT a, UINT b, INT c), (ctx, a, b, c))
VU_DRAW_HOOK(Hook_DrawInstanced, g_oDrawInstanced, (ID3D11DeviceContext *ctx, UINT a, UINT b, UINT c, UINT d), (ctx, a, b, c, d))
VU_DRAW_HOOK(Hook_DrawIndexedInstanced, g_oDrawIndexedInstanced,
             (ID3D11DeviceContext *ctx, UINT a, UINT b, UINT c, INT d, UINT e), (ctx, a, b, c, d, e))
VU_DRAW_HOOK(Hook_DrawAuto, g_oDrawAuto, (ID3D11DeviceContext *ctx), (ctx))
VU_DRAW_HOOK(Hook_DrawInstancedIndirect, g_oDrawInstancedIndirect, (ID3D11DeviceContext *ctx, ID3D11Buffer *buf, UINT off),
             (ctx, buf, off))
VU_DRAW_HOOK(Hook_DrawIndexedInstancedIndirect, g_oDrawIndexedInstancedIndirect,
             (ID3D11DeviceContext *ctx, ID3D11Buffer *buf, UINT off), (ctx, buf, off))
#undef VU_DRAW_HOOK

template <int V>
void STDMETHODCALLTYPE Hook_ClearState(ID3D11DeviceContext *ctx)
{
    if (g_shuttingDown || InVu()) return g_oClearState[V](ctx);
    VuScope scope;
    g_oClearState[V](ctx);
    OnClearState(ctx);
}

template <int V>
void STDMETHODCALLTYPE Hook_CopyResource(ID3D11DeviceContext *ctx, ID3D11Resource *dst, ID3D11Resource *src)
{
    if (g_shuttingDown || InVu()) return g_oCopyResource[V](ctx, dst, src);
    VuScope scope;
    InterlockedIncrement(&g_stats.copyHookCalls);
    g_oCopyResource[V](ctx, dst, src);
    OnCopy(ctx, dst, src);
}

template <int V>
void STDMETHODCALLTYPE Hook_CopySubresourceRegion(ID3D11DeviceContext *ctx, ID3D11Resource *dst, UINT dsub, UINT x, UINT y,
                                                  UINT z, ID3D11Resource *src, UINT ssub, const D3D11_BOX *box)
{
    if (g_shuttingDown || InVu()) return g_oCopySubresourceRegion[V](ctx, dst, dsub, x, y, z, src, ssub, box);
    VuScope scope;
    InterlockedIncrement(&g_stats.copyHookCalls);
    g_oCopySubresourceRegion[V](ctx, dst, dsub, x, y, z, src, ssub, box);
    OnCopy(ctx, dst, src);
}

template <int V>
void STDMETHODCALLTYPE Hook_CopySubresourceRegion1(ID3D11DeviceContext1 *ctx, ID3D11Resource *dst, UINT dsub, UINT x, UINT y,
                                                   UINT z, ID3D11Resource *src, UINT ssub, const D3D11_BOX *box, UINT flags)
{
    if (g_shuttingDown || InVu()) return g_oCopySubresourceRegion1[V](ctx, dst, dsub, x, y, z, src, ssub, box, flags);
    VuScope scope;
    InterlockedIncrement(&g_stats.copyHookCalls);
    g_oCopySubresourceRegion1[V](ctx, dst, dsub, x, y, z, src, ssub, box, flags);
    OnCopy(ctx, dst, src);
}

// Command lists: a deferred context's state resets on FinishCommandList(FALSE), any context's on
// ExecuteCommandList(..., FALSE). The upscales a command list's draws need are attached to it at
// FinishCommandList and run on the immediate context just before it executes, together with our
// housekeeping when the compositor renders through command lists.
template <int V>
HRESULT STDMETHODCALLTYPE Hook_FinishCommandList(ID3D11DeviceContext *ctx, BOOL restore, ID3D11CommandList **pp)
{
    if (g_shuttingDown || InVu()) return g_oFinishCommandList[V](ctx, restore, pp);
    VuScope scope;
    HRESULT hr = g_oFinishCommandList[V](ctx, restore, pp);
    { HookCpu cpu; OnFinishCommandList(ctx, restore != FALSE, (SUCCEEDED(hr) && pp) ? *pp : nullptr); }
    return hr;
}

template <int V>
void STDMETHODCALLTYPE Hook_ExecuteCommandList(ID3D11DeviceContext *ctx, ID3D11CommandList *cl, BOOL restore)
{
    if (g_shuttingDown || InVu()) return g_oExecuteCommandList[V](ctx, cl, restore);
    VuScope scope;
    { HookCpu cpu; OnBeforeExecuteCommandList(ctx, cl); }
    g_oExecuteCommandList[V](ctx, cl, restore);
    { HookCpu cpu; OnAfterExecuteCommandList(ctx, restore != FALSE); }
}

// ---- method table -------------------------------------------------------------------------------

enum CtxMethod {
    M_PS, M_CS, M_Draw, M_DrawIndexed, M_DrawInstanced, M_DrawIndexedInstanced, M_DrawAuto, M_DrawInstancedIndirect,
    M_DrawIndexedInstancedIndirect, M_ClearState, M_CopyResource, M_CopySubresourceRegion, M_CopySubresourceRegion1,
    M_FinishCommandList, M_ExecuteCommandList, M_COUNT
};

struct MethodVariants {
    const char *name;
    void *detour[kVariants];
    void **orig[kVariants];
    void *body[kVariants];
    int n;
    bool fullLogged;
};

#define VU_VARIANTS(NAME, HOOK, ARR)                                                                              \
    {                                                                                                             \
        NAME,                                                                                                     \
        { (void *)&HOOK<0>, (void *)&HOOK<1>, (void *)&HOOK<2>, (void *)&HOOK<3>, (void *)&HOOK<4>, (void *)&HOOK<5> }, \
        { (void **)&ARR[0], (void **)&ARR[1], (void **)&ARR[2], (void **)&ARR[3], (void **)&ARR[4], (void **)&ARR[5] },  \
        {}, 0, false                                                                                              \
    }

MethodVariants g_m[M_COUNT] = {
    VU_VARIANTS("PSSetShaderResources", Hook_PSSetShaderResources, g_oPS),
    VU_VARIANTS("CSSetShaderResources", Hook_CSSetShaderResources, g_oCS),
    VU_VARIANTS("Draw", Hook_Draw, g_oDraw),
    VU_VARIANTS("DrawIndexed", Hook_DrawIndexed, g_oDrawIndexed),
    VU_VARIANTS("DrawInstanced", Hook_DrawInstanced, g_oDrawInstanced),
    VU_VARIANTS("DrawIndexedInstanced", Hook_DrawIndexedInstanced, g_oDrawIndexedInstanced),
    VU_VARIANTS("DrawAuto", Hook_DrawAuto, g_oDrawAuto),
    VU_VARIANTS("DrawInstancedIndirect", Hook_DrawInstancedIndirect, g_oDrawInstancedIndirect),
    VU_VARIANTS("DrawIndexedInstancedIndirect", Hook_DrawIndexedInstancedIndirect, g_oDrawIndexedInstancedIndirect),
    VU_VARIANTS("ClearState", Hook_ClearState, g_oClearState),
    VU_VARIANTS("CopyResource", Hook_CopyResource, g_oCopyResource),
    VU_VARIANTS("CopySubresourceRegion", Hook_CopySubresourceRegion, g_oCopySubresourceRegion),
    VU_VARIANTS("CopySubresourceRegion1", Hook_CopySubresourceRegion1, g_oCopySubresourceRegion1),
    VU_VARIANTS("FinishCommandList", Hook_FinishCommandList, g_oFinishCommandList),
    VU_VARIANTS("ExecuteCommandList", Hook_ExecuteCommandList, g_oExecuteCommandList),
};
#undef VU_VARIANTS

void BodiesOf(const VuCtxFns &cf, void *b[M_COUNT])
{
    b[M_PS] = cf.PSSetShaderResources;
    b[M_CS] = cf.CSSetShaderResources;
    b[M_Draw] = cf.Draw;
    b[M_DrawIndexed] = cf.DrawIndexed;
    b[M_DrawInstanced] = cf.DrawInstanced;
    b[M_DrawIndexedInstanced] = cf.DrawIndexedInstanced;
    b[M_DrawAuto] = cf.DrawAuto;
    b[M_DrawInstancedIndirect] = cf.DrawInstancedIndirect;
    b[M_DrawIndexedInstancedIndirect] = cf.DrawIndexedInstancedIndirect;
    b[M_ClearState] = cf.ClearState;
    b[M_CopyResource] = cf.CopyResource;
    b[M_CopySubresourceRegion] = cf.CopySubresourceRegion;
    b[M_CopySubresourceRegion1] = cf.CopySubresourceRegion1;
    b[M_FinishCommandList] = cf.FinishCommandList;
    b[M_ExecuteCommandList] = cf.ExecuteCommandList;
}

// "d3d11.dll+0x1234" for log lines
void Where(void *p, char *out, size_t n)
{
    HMODULE m = nullptr;
    if (p && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                (LPCWSTR)p, &m) && m) {
        wchar_t w[MAX_PATH];
        DWORD k = GetModuleFileNameW(m, w, MAX_PATH);
        const wchar_t *base = w;
        for (DWORD i = 0; i < k; ++i)
            if (w[i] == L'\\' || w[i] == L'/') base = w + i + 1;
        char name[MAX_PATH];
        WideCharToMultiByte(CP_UTF8, 0, base, -1, name, sizeof(name), nullptr, nullptr);
        snprintf(out, n, "%s+0x%llx", name, (unsigned long long)((uintptr_t)p - (uintptr_t)m));
    } else {
        snprintf(out, n, "%p", p);
    }
}

CRITICAL_SECTION g_installLock;
INIT_ONCE g_installLockOnce = INIT_ONCE_STATIC_INIT;

BOOL CALLBACK InitInstallLock(PINIT_ONCE, PVOID, PVOID *)
{
    InitializeCriticalSection(&g_installLock);
    if (g_tls == TLS_OUT_OF_INDEXES) g_tls = TlsAlloc();
    return TRUE;
}

void EnsureInstallLock() { InitOnceExecuteOnce(&g_installLockOnce, InitInstallLock, nullptr, nullptr); }

bool EnsureMinHook()
{
    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
        LOGE("hooks: MH_Initialize failed (%d)", (int)st);
        return false;
    }
    return true;
}

bool KnownBody(int m, void *b)
{
    for (int v = 0; v < g_m[m].n; ++v)
        if (g_m[m].body[v] == b) return true;
    return false;
}

// Hooks every context-method body of `ctx` not hooked yet. Returns the number of new bodies.
// Caller holds g_installLock.
int AdoptLocked(ID3D11DeviceContext *ctx, const char *what, bool first)
{
    Ref<ID3D11DeviceContext1> ctx1;
    ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void **)ctx1.Put());
    VuCtxFns cf = {};
    vu_get_ctx_fns(ctx, ctx1.Get(), &cf);
    void *b[M_COUNT];
    BodiesOf(cf, b);
    // FinishCommandList on an immediate context is an error stub, which a runtime may share
    // between unrelated methods. (A deferred context can execute a command list inside another.)
    if (ctx->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE) b[M_FinishCommandList] = nullptr;

    // If the runtime folded two methods into one body, a detour written for one signature would
    // receive calls meant for the other: never hook a body that fills more than one slot of the
    // context's vtable.
    bool dup[M_COUNT] = {};
    for (int i = 0; i < M_COUNT; ++i) {
        if (!b[i]) continue;
        int uses = 0;
        for (int k = 0; k < cf.vtblCount; ++k)
            if (cf.vtbl[k] == b[i]) ++uses;
        if (uses > 1) dup[i] = true;
        for (int j = 0; j < M_COUNT; ++j)
            if (j != i && b[j] == b[i]) dup[i] = true;
    }

    int added = 0;
    char names[512] = "";
    size_t len = 0;
    void *firstNew = nullptr;
    for (int i = 0; i < M_COUNT; ++i) {
        if (!b[i] || KnownBody(i, b[i])) continue;
        MethodVariants &m = g_m[i];
        if (dup[i]) {
            LOGE("hooks: %s of %s shares its body with another method; not hooked", m.name, what);
            continue;
        }
        if (m.n >= kVariants) {
            if (!m.fullLogged) LOGE("hooks: %s: more than %d distinct method bodies; %s is not hooked", m.name, kVariants, what);
            m.fullLogged = true;
            continue;
        }
        const int v = m.n;
        MH_STATUS st = MH_CreateHook(b[i], m.detour[v], (void **)m.orig[v]);
        if (st == MH_OK) st = MH_QueueEnableHook(b[i]);
        if (st != MH_OK) {
            char at[160];
            Where(b[i], at, sizeof(at));
            LOGE("hooks: could not hook %s of %s at %s (%d)", m.name, what, at, (int)st);
            MH_RemoveHook(b[i]);
            continue;
        }
        m.body[v] = b[i];
        m.n = v + 1;   // the trampoline is set; the detour runs only once the hook is applied
        ++added;
        if (!firstNew) firstNew = b[i];
        if (len < sizeof(names) - 40)
            len += snprintf(names + len, sizeof(names) - len, "%s%s", len ? ", " : "", m.name);
    }
    if (added == 0) return 0;
    MH_STATUS st = MH_ApplyQueued();
    if (st != MH_OK) LOGE("hooks: enabling %d hooks for %s failed (%d)", added, what, (int)st);
    char at[160];
    Where(firstNew, at, sizeof(at));
    if (first) {
        LOGI("hooks: %d D3D11 hooks active (method bodies taken from %s; PSSetShaderResources at %s)", added, what, at);
    } else {
        InterlockedAdd(&g_stats.extraBodies, added);
        LOGI("hooks: %s uses %d method bodies not seen before (%s; first at %s); hooked those too", what, added, names, at);
    }
    return added;
}

int Adopt(ID3D11DeviceContext *ctx, const char *what)
{
    if (!ctx || !g_installed) return 0;
    EnsureInstallLock();
    EnterCriticalSection(&g_installLock);
    int n = AdoptLocked(ctx, what, false);
    LeaveCriticalSection(&g_installLock);
    return n;
}

void AdoptDevice(ID3D11Device *dev, const char *what)
{
    if (!dev || !g_installed) return;
    Ref<ID3D11DeviceContext> ctx;
    dev->GetImmediateContext(ctx.Put());
    if (ctx) Adopt(ctx.Get(), what);
}

// ---- passthrough calls for our own code (state restore inside a detour) -----------------------

void STDMETHODCALLTYPE CallPSSetShaderResources(ID3D11DeviceContext *ctx, UINT start, UINT num,
                                                ID3D11ShaderResourceView *const *views)
{
    VuScope scope;
    ctx->PSSetShaderResources(start, num, views);
}

void STDMETHODCALLTYPE CallCSSetShaderResources(ID3D11DeviceContext *ctx, UINT start, UINT num,
                                                ID3D11ShaderResourceView *const *views)
{
    VuScope scope;
    ctx->CSSetShaderResources(start, num, views);
}

// ---- device-method detours ---------------------------------------------------------------------

volatile LONG g_deferredLogBudget = 4;
volatile LONG g_mtLogBudget = 8;

void *g_openDevs[8];   // devices already named in the log (identity only, never dereferenced)
SRWLOCK g_openDevsLock = SRWLOCK_INIT;

// Every shared texture the compositor opens is about to be drawn with: make sure the device's
// immediate context still runs through hooked bodies (it may have switched variants since).
void AfterOpen(ID3D11Device *dev, IUnknown *obj, REFIID riid, const char *how)
{
    if (g_shuttingDown || InVu()) return;
    VuScope scope;
    OnSharedOpened(dev, obj, riid, how);
    AdoptDevice(dev, "the compositor's immediate context (checked at a shared-texture open)");

    bool fresh = false;
    AcquireSRWLockExclusive(&g_openDevsLock);
    int i = 0;
    while (i < 8 && g_openDevs[i] && g_openDevs[i] != dev) ++i;
    if (i < 8 && !g_openDevs[i]) { g_openDevs[i] = dev; fresh = true; }
    ReleaseSRWLockExclusive(&g_openDevsLock);
    if (fresh) {
        Ref<ID3D11DeviceContext> ctx;
        dev->GetImmediateContext(ctx.Put());
        char at[160] = "?";
        if (ctx) {
            VuCtxFns cf = {};
            vu_get_ctx_fns(ctx.Get(), nullptr, &cf);
            Where(cf.PSSetShaderResources, at, sizeof(at));
        }
        LOGI("hooks: device %p opens shared textures; its immediate context is %p (PSSetShaderResources at %s)", (void *)dev,
             (void *)ctx.Get(), at);
    }
}

HRESULT STDMETHODCALLTYPE Hook_OpenSharedResource(ID3D11Device *dev, HANDLE h, REFIID riid, void **pp)
{
    HRESULT hr = g_origOpenSharedResource(dev, h, riid, pp);
    if (SUCCEEDED(hr) && pp && *pp) AfterOpen(dev, (IUnknown *)*pp, riid, "OpenSharedResource");
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_OpenSharedResource1(ID3D11Device1 *dev, HANDLE h, REFIID riid, void **pp)
{
    HRESULT hr = g_origOpenSharedResource1(dev, h, riid, pp);
    if (SUCCEEDED(hr) && pp && *pp) AfterOpen(dev, (IUnknown *)*pp, riid, "OpenSharedResource1");
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_OpenSharedResourceByName(ID3D11Device1 *dev, LPCWSTR name, DWORD access, REFIID riid, void **pp)
{
    HRESULT hr = g_origOpenSharedResourceByName(dev, name, access, riid, pp);
    if (SUCCEEDED(hr) && pp && *pp) AfterOpen(dev, (IUnknown *)*pp, riid, "OpenSharedResourceByName");
    return hr;
}

void AfterDeferred(ID3D11Device *dev, ID3D11DeviceContext *dc)
{
    if (g_shuttingDown || InVu() || !dc) return;
    VuScope scope;
    if (InterlockedDecrement(&g_deferredLogBudget) >= 0)
        LOGI("hooks: the compositor created deferred context %p on device %p", (void *)dc, (void *)dev);
    Adopt(dc, "a deferred context");
}

HRESULT STDMETHODCALLTYPE Hook_CreateDeferredContext(ID3D11Device *dev, UINT flags, ID3D11DeviceContext **pp)
{
    HRESULT hr = g_origCreateDeferredContext(dev, flags, pp);
    if (SUCCEEDED(hr) && pp && *pp) AfterDeferred(dev, *pp);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateDeferredContext1(ID3D11Device1 *dev, UINT flags, ID3D11DeviceContext1 **pp)
{
    HRESULT hr = g_origCreateDeferredContext1(dev, flags, pp);
    if (SUCCEEDED(hr) && pp && *pp) AfterDeferred(dev, *pp);
    return hr;
}

// Multithread protection can switch a context to other method bodies: re-check right after. One
// body may serve both ID3D11Multithread (on a context) and ID3D10Multithread (on a device), so
// the detour accepts either.
template <int V>
BOOL STDMETHODCALLTYPE Hook_SetMT(IUnknown *self, BOOL on)
{
    BOOL was = g_origSetMT[V](self, on);
    if (!g_shuttingDown && !InVu()) {
        VuScope scope;
        Ref<ID3D11DeviceContext> ctx;
        if (FAILED(self->QueryInterface(__uuidof(ID3D11DeviceContext), (void **)ctx.Put())) || !ctx) {
            Ref<ID3D11Device> dev;
            if (SUCCEEDED(self->QueryInterface(__uuidof(ID3D11Device), (void **)dev.Put())) && dev)
                dev->GetImmediateContext(ctx.Put());
        }
        if (InterlockedDecrement(&g_mtLogBudget) >= 0)
            LOGI("hooks: SetMultithreadProtected(%d) on context %p (was %d)", (int)on, (void *)ctx.Get(), (int)was);
        if (ctx) Adopt(ctx.Get(), "a context after SetMultithreadProtected");
    }
    return was;
}

struct HookSpec {
    const char *name;
    void *target;
    void *detour;
    void **original;
};

} // namespace

bool HooksInstalled() { return g_installed != 0; }

namespace {

// First installation: device-method hooks plus the context bodies of this device.
bool InstallFrom(ID3D11Device *dev, ID3D11DeviceContext *ctx, const char *source)
{
    EnsureInstallLock();
    EnterCriticalSection(&g_installLock);
    if (g_installed) {
        int n = AdoptLocked(ctx, source, false);
        if (n == 0) LOGV("hooks: device from %s runs through the hooked method bodies", source);
        LeaveCriticalSection(&g_installLock);
        return true;
    }
    if (!EnsureMinHook()) {
        LeaveCriticalSection(&g_installLock);
        return false;
    }

    Ref<ID3D11Device1> dev1;
    dev->QueryInterface(__uuidof(ID3D11Device1), (void **)dev1.Put());
    VuDevFns df = {};
    vu_get_dev_fns(dev, dev1.Get(), &df);
    Ref<ID3D11Multithread> mt11;
    ctx->QueryInterface(__uuidof(ID3D11Multithread), (void **)mt11.Put());
    Ref<ID3D10Multithread> mt10;
    dev->QueryInterface(__uuidof(ID3D10Multithread), (void **)mt10.Put());
    VuMtFns mf = {};
    vu_get_mt_fns(mt11.Get(), mt10.Get(), &mf);

    HookSpec specs[] = {
        { "OpenSharedResource", df.OpenSharedResource, (void *)&Hook_OpenSharedResource, (void **)&g_origOpenSharedResource },
        { "OpenSharedResource1", df.OpenSharedResource1, (void *)&Hook_OpenSharedResource1, (void **)&g_origOpenSharedResource1 },
        { "OpenSharedResourceByName", df.OpenSharedResourceByName, (void *)&Hook_OpenSharedResourceByName, (void **)&g_origOpenSharedResourceByName },
        { "CreateDeferredContext", df.CreateDeferredContext, (void *)&Hook_CreateDeferredContext, (void **)&g_origCreateDeferredContext },
        { "CreateDeferredContext1", df.CreateDeferredContext1, (void *)&Hook_CreateDeferredContext1, (void **)&g_origCreateDeferredContext1 },
        { "ID3D11Multithread::SetMultithreadProtected", mf.SetMultithreadProtected11, (void *)&Hook_SetMT<0>, (void **)&g_origSetMT[0] },
        { "ID3D10Multithread::SetMultithreadProtected",
          mf.SetMultithreadProtected10 != mf.SetMultithreadProtected11 ? mf.SetMultithreadProtected10 : nullptr,
          (void *)&Hook_SetMT<1>, (void **)&g_origSetMT[1] },
    };
    const int count = (int)(sizeof(specs) / sizeof(specs[0]));
    for (int i = 0; i < count; ++i) {
        HookSpec &s = specs[i];
        if (!s.target) {
            LOGV("hooks: %s not available on this runtime", s.name);
            continue;
        }
        bool shared = false;
        for (int j = 0; j < count; ++j)
            if (j != i && specs[j].target == s.target) shared = true;
        if (shared) {
            LOGE("hooks: %s shares its body with another hooked method; skipped", s.name);
            continue;
        }
        MH_STATUS st = MH_CreateHook(s.target, s.detour, s.original);
        if (st == MH_OK) st = MH_QueueEnableHook(s.target);
        if (st != MH_OK) {
            LOGE("hooks: could not hook %s @ %p (%d)", s.name, s.target, (int)st);
            MH_RemoveHook(s.target);
            *s.original = nullptr;
        }
    }

    int n = AdoptLocked(ctx, source, true);
    // Without these two the upscaler cannot work at all; without the rest it degrades.
    if (n == 0 || g_m[M_PS].n == 0 || g_m[M_Draw].n == 0) {
        LOGE("hooks: essential hooks missing; leaving the compositor untouched");
        for (auto &m : g_m) {
            for (int v = 0; v < m.n; ++v) MH_RemoveHook(m.body[v]);
            m.n = 0;
        }
        for (int i = 0; i < count; ++i)
            if (specs[i].target && *specs[i].original) {
                MH_RemoveHook(specs[i].target);
                *specs[i].original = nullptr;
            }
        LeaveCriticalSection(&g_installLock);
        return false;
    }
    if (MH_ApplyQueued() != MH_OK) LOGE("hooks: some device-method hooks could not be enabled");
    g_origPSSetShaderResources = &CallPSSetShaderResources;
    g_origCSSetShaderResources = &CallCSSetShaderResources;
    InterlockedExchange(&g_installed, 1);
    InterlockedExchange(&g_stats.hooksInstalled, 1);
    LeaveCriticalSection(&g_installLock);
    return true;
}

// ---- D3D11CreateDevice / D3D11CreateDeviceAndSwapChain export hooks --------------------------

typedef HRESULT (WINAPI *PFN_D3D11CreateDeviceAndSwapChain)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT,
                                                           const D3D_FEATURE_LEVEL *, UINT, UINT, const DXGI_SWAP_CHAIN_DESC *,
                                                           IDXGISwapChain **, ID3D11Device **, D3D_FEATURE_LEVEL *,
                                                           ID3D11DeviceContext **);
PFN_D3D11CreateDevice g_origCreateDevice = nullptr;
PFN_D3D11CreateDeviceAndSwapChain g_origCreateDeviceAndSwapChain = nullptr;
volatile LONG g_exportsHooked = 0;
volatile LONG g_inProbe = 0;
volatile LONG g_probeDone = 0;

void AfterDeviceCreated(HRESULT hr, ID3D11Device **ppDev, ID3D11DeviceContext **ppCtx, UINT flags, const char *fn)
{
    if (FAILED(hr) || !ppDev || !*ppDev || InVu()) return;
    VuScope scope;
    ID3D11Device *dev = *ppDev;
    Ref<ID3D11DeviceContext> ctx;
    if (ppCtx && *ppCtx) ctx = Ref<ID3D11DeviceContext>(*ppCtx, true);
    else dev->GetImmediateContext(ctx.Put());
    if (!g_inProbe)
        LOGI("hooks: %s -> device %p (flags 0x%x, feature level 0x%x)", fn, (void *)dev, flags,
             (unsigned)dev->GetFeatureLevel());
    if (ctx) InstallFrom(dev, ctx.Get(), g_inProbe ? "the probe device" : "the compositor's own device");
}

HRESULT WINAPI Hook_D3D11CreateDevice(IDXGIAdapter *ad, D3D_DRIVER_TYPE dt, HMODULE sw, UINT flags,
                                      const D3D_FEATURE_LEVEL *fls, UINT nfl, UINT sdk, ID3D11Device **ppDev,
                                      D3D_FEATURE_LEVEL *pfl, ID3D11DeviceContext **ppCtx)
{
    HRESULT hr = g_origCreateDevice(ad, dt, sw, flags, fls, nfl, sdk, ppDev, pfl, ppCtx);
    if (!g_shuttingDown) AfterDeviceCreated(hr, ppDev, ppCtx, flags, "D3D11CreateDevice");
    return hr;
}

HRESULT WINAPI Hook_D3D11CreateDeviceAndSwapChain(IDXGIAdapter *ad, D3D_DRIVER_TYPE dt, HMODULE sw, UINT flags,
                                                  const D3D_FEATURE_LEVEL *fls, UINT nfl, UINT sdk,
                                                  const DXGI_SWAP_CHAIN_DESC *scd, IDXGISwapChain **ppSc,
                                                  ID3D11Device **ppDev, D3D_FEATURE_LEVEL *pfl, ID3D11DeviceContext **ppCtx)
{
    HRESULT hr = g_origCreateDeviceAndSwapChain(ad, dt, sw, flags, fls, nfl, sdk, scd, ppSc, ppDev, pfl, ppCtx);
    if (!g_shuttingDown) AfterDeviceCreated(hr, ppDev, ppCtx, flags, "D3D11CreateDeviceAndSwapChain");
    return hr;
}

PFN_D3D11CreateDevice RawCreateDevice()
{
    if (g_origCreateDevice) return g_origCreateDevice;   // trampoline: bypasses our export hook
    HMODULE d3d11 = GetModuleHandleW(L"d3d11.dll");
    return d3d11 ? (PFN_D3D11CreateDevice)GetProcAddress(d3d11, "D3D11CreateDevice") : nullptr;
}

} // namespace

bool HookDeviceCreation()
{
    if (g_exportsHooked) return true;
    EnsureInstallLock();
    HMODULE d3d11 = GetModuleHandleW(L"d3d11.dll");
    if (!d3d11) return false;
    if (!EnsureMinHook()) return false;
    void *a = (void *)GetProcAddress(d3d11, "D3D11CreateDevice");
    void *b = (void *)GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain");
    int n = 0;
    if (a && MH_CreateHook(a, (void *)&Hook_D3D11CreateDevice, (void **)&g_origCreateDevice) == MH_OK &&
        MH_EnableHook(a) == MH_OK)
        ++n;
    if (b && MH_CreateHook(b, (void *)&Hook_D3D11CreateDeviceAndSwapChain, (void **)&g_origCreateDeviceAndSwapChain) == MH_OK &&
        MH_EnableHook(b) == MH_OK)
        ++n;
    InterlockedExchange(&g_exportsHooked, 1);
    LOGI("hooks: watching D3D11 device creation (%d export hooks)", n);
    return n > 0;
}

// Fallback when the compositor's device was created before we could watch for it (late
// injection, or d3d11.dll loaded dynamically): take the method bodies from a throwaway device.
bool InstallD3D11Hooks()
{
    if (g_installed) return true;
    EnsureInstallLock();
    auto create = RawCreateDevice();
    if (!create) {
        LOGE("hooks: D3D11CreateDevice not found");
        return false;
    }
    Ref<ID3D11Device> dev;
    Ref<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL fl = {};
    InterlockedExchange(&g_inProbe, 1);
    HRESULT hr = create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, dev.Put(), &fl, ctx.Put());
    const char *driver = "hardware";
    if (FAILED(hr)) {
        LOGI("hooks: hardware probe device failed (0x%08lx), trying WARP", (unsigned long)hr);
        hr = create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, dev.Put(), &fl, ctx.Put());
        driver = "warp";
    }
    InterlockedExchange(&g_inProbe, 0);
    if (FAILED(hr) || !dev || !ctx) {
        LOGE("hooks: could not create a probe D3D11 device (0x%08lx)", (unsigned long)hr);
        return false;
    }
    LOGI("hooks: probe device created (%s, feature level 0x%x)", driver, (unsigned)fl);
    return InstallFrom(dev.Get(), ctx.Get(), "a probe device");
}

// Runs once on the worker thread after the hooks are in: finds the method bodies the runtime uses
// in other context configurations (multithread-protected, single-threaded) and hooks them up
// front, and logs whether protection switches bodies at all (the log answers that per machine).
void ProbeContextVariants()
{
    if (!g_installed || InterlockedExchange(&g_probeDone, 1)) return;
    auto create = RawCreateDevice();
    if (!create) return;
    VuScope scope;   // our own calls below must not run the upscaler
    InterlockedExchange(&g_inProbe, 1);
    for (int pass = 0; pass < 2; ++pass) {
        const UINT flags = pass == 0 ? 0u : (UINT)D3D11_CREATE_DEVICE_SINGLETHREADED;
        Ref<ID3D11Device> dev;
        Ref<ID3D11DeviceContext> ctx;
        D3D_FEATURE_LEVEL fl = {};
        HRESULT hr = create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, dev.Put(), &fl,
                            ctx.Put());
        if (FAILED(hr) || !ctx) {
            LOGV("hooks: probe device (flags 0x%x) failed (0x%08lx)", flags, (unsigned long)hr);
            continue;
        }
        Ref<ID3D11DeviceContext1> c1;
        ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void **)c1.Put());
        VuCtxFns before = {};
        vu_get_ctx_fns(ctx.Get(), c1.Get(), &before);
        int n = Adopt(ctx.Get(), pass == 0 ? "a probe device" : "a single-threaded probe device");
        if (pass == 1) {
            LOGI("hooks: probe: a single-threaded device %s", n ? "uses other method bodies (hooked)" : "uses the same method bodies");
            continue;
        }
        // Deferred contexts are their own class: hook them now, so that a compositor recording
        // its rendering into command lists shows up in the log (they are not upscaled yet).
        Ref<ID3D11DeviceContext> dc;
        if (SUCCEEDED(dev->CreateDeferredContext(0, dc.Put())) && dc) {
            int k = Adopt(dc.Get(), "a probe deferred context");
            LOGI("hooks: probe: deferred contexts %s", k ? "use other method bodies (hooked, for diagnostics)" : "use the same method bodies");
        }
        Ref<ID3D11Multithread> mt;
        ctx->QueryInterface(__uuidof(ID3D11Multithread), (void **)mt.Put());
        if (!mt) {
            LOGI("hooks: probe: ID3D11Multithread not available");
            continue;
        }
        int was = vu_mt11_get(mt.Get());
        vu_mt11_set(mt.Get(), !was);   // flip protection (passes through our hook: guarded)
        VuCtxFns after = {};
        vu_get_ctx_fns(ctx.Get(), c1.Get(), &after);
        bool changed = memcmp(&before, &after, sizeof(before)) != 0;
        int m = Adopt(ctx.Get(), "a probe device after flipping multithread protection");
        LOGI("hooks: probe: switching multithread protection %s -> %s %s", was ? "on" : "off", was ? "off" : "on",
             changed ? (m ? "switches the context to other method bodies (hooked those too)"
                          : "switches the context to other, already hooked method bodies")
                     : "keeps the same method bodies");
        vu_mt11_set(mt.Get(), was);
    }
    InterlockedExchange(&g_inProbe, 0);
}

// "PS binds by hooked body: 0:1200 1:53000 | draws: ..." when more than one body was hooked.
// True if the bodies that carry queued command-list work are hooked: FinishCommandList of this
// deferred context and ExecuteCommandList of its device's immediate context. (A body can be left
// unhooked: shared with another method, too many variants, or MinHook failed.)
bool CommandListHooksLive(ID3D11DeviceContext *deferred)
{
    if (!deferred || !g_installed) return false;
    VuCtxFns cf = {};
    vu_get_ctx_fns(deferred, nullptr, &cf);
    if (!KnownBody(M_FinishCommandList, cf.FinishCommandList)) return false;
    Ref<ID3D11Device> dev;
    deferred->GetDevice(dev.Put());
    Ref<ID3D11DeviceContext> imm;
    if (dev) dev->GetImmediateContext(imm.Put());
    if (!imm) return false;
    VuCtxFns ci = {};
    vu_get_ctx_fns(imm.Get(), nullptr, &ci);
    return KnownBody(M_ExecuteCommandList, ci.ExecuteCommandList);
}

void FormatHookVariantHits(char *out, size_t n)
{
    out[0] = 0;
    if (g_m[M_PS].n < 2 && g_m[M_Draw].n < 2) return;
    size_t len = (size_t)snprintf(out, n, "PS binds by hooked body:");
    for (int v = 0; v < g_m[M_PS].n && len < n; ++v) len += snprintf(out + len, n - len, " %d:%ld", v, (long)g_psHits[v]);
    if (len < n) len += snprintf(out + len, n - len, " | draws:");
    for (int v = 0; v < g_m[M_Draw].n && len < n; ++v) len += snprintf(out + len, n - len, " %d:%ld", v, (long)g_drawHits[v]);
}

} // namespace vu
