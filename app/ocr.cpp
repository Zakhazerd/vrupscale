// Windows' OCR through its WinRT interfaces, by vtable slot (see ocr.h).
#include "ocr.h"
#include "core.h"

#include <unknwn.h>
#include <string.h>

namespace app {

namespace {

// ---- the Windows Runtime's C entry points (combase.dll) --------------------------------------------

typedef void *HStr;   // HSTRING
typedef HRESULT(WINAPI *PFN_RoInitialize)(int type);
typedef void(WINAPI *PFN_RoUninitialize)();
typedef HRESULT(WINAPI *PFN_RoGetActivationFactory)(HStr classId, REFIID iid, void **factory);
typedef HRESULT(WINAPI *PFN_WindowsCreateString)(const wchar_t *src, UINT32 len, HStr *out);
typedef HRESULT(WINAPI *PFN_WindowsDeleteString)(HStr s);
typedef const wchar_t *(WINAPI *PFN_WindowsGetStringRawBuffer)(HStr s, UINT32 *len);

struct Rt {
    PFN_RoInitialize init = nullptr;
    PFN_RoUninitialize uninit = nullptr;
    PFN_RoGetActivationFactory factory = nullptr;
    PFN_WindowsCreateString create = nullptr;
    PFN_WindowsDeleteString del = nullptr;
    PFN_WindowsGetStringRawBuffer raw = nullptr;
    bool ok = false;
};

const Rt &Runtime()
{
    static Rt rt;
    static bool tried = false;
    if (tried) return rt;
    tried = true;
    HMODULE m = LoadLibraryExW(L"combase.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!m) return rt;
    rt.init = (PFN_RoInitialize)GetProcAddress(m, "RoInitialize");
    rt.uninit = (PFN_RoUninitialize)GetProcAddress(m, "RoUninitialize");
    rt.factory = (PFN_RoGetActivationFactory)GetProcAddress(m, "RoGetActivationFactory");
    rt.create = (PFN_WindowsCreateString)GetProcAddress(m, "WindowsCreateString");
    rt.del = (PFN_WindowsDeleteString)GetProcAddress(m, "WindowsDeleteString");
    rt.raw = (PFN_WindowsGetStringRawBuffer)GetProcAddress(m, "WindowsGetStringRawBuffer");
    rt.ok = rt.init && rt.uninit && rt.factory && rt.create && rt.del && rt.raw;
    return rt;
}

// ---- interfaces: IUnknown is slots 0-2, IInspectable 3-5, an interface's own methods from 6 --------
// (IDs and method order as Windows' metadata gives them: Windows.Media.winmd, Windows.Graphics.winmd,
// Windows.Globalization.winmd, Windows.Foundation.winmd)

const GUID kIOcrEngineStatics = { 0x5bffa85a, 0x3384, 0x3540, { 0x99, 0x40, 0x69, 0x91, 0x20, 0xd4, 0x28, 0xa8 } };
const GUID kILanguageFactory = { 0x9b0252ac, 0x0c27, 0x44f8, { 0xb7, 0x92, 0x97, 0x93, 0xfb, 0x66, 0xc6, 0x3e } };
const GUID kISoftwareBitmapFactory = { 0xc99feb69, 0x2d62, 0x4d47, { 0xa6, 0xb3, 0x4f, 0xdb, 0x6a, 0x07, 0xfd, 0xf8 } };
const GUID kIMemoryBuffer = { 0xfbc4dd2a, 0x245b, 0x11e4, { 0xaf, 0x98, 0x68, 0x94, 0x23, 0x26, 0x0c, 0xf8 } };
const GUID kIMemoryBufferByteAccess = { 0x5b0d3235, 0x4dba, 0x4d44, { 0x86, 0x5e, 0x8f, 0x1d, 0x0e, 0x4f, 0xd0, 0x4d } };
const GUID kIAsyncInfo = { 0x00000036, 0x0000, 0x0000, { 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
const GUID kIClosable = { 0x30d5a829, 0x7fa4, 0x4026, { 0x83, 0xbb, 0xd7, 0x5b, 0xae, 0x4e, 0xa9, 0x9e } };

enum {
    // IOcrEngineStatics
    kStaticsMaxDimension = 6, kStaticsIsSupported = 8, kStaticsTryCreate = 9,
    // IOcrEngine
    kEngineRecognizeAsync = 6,
    // IOcrResult
    kResultLines = 6, kResultTextAngle = 7,
    // IOcrLine
    kLineWords = 6,
    // IOcrWord
    kWordRect = 6, kWordText = 7,
    // ILanguageFactory
    kLanguageCreate = 6,
    // ISoftwareBitmapFactory
    kBitmapCreate = 6,
    // ISoftwareBitmap
    kBitmapLockBuffer = 15,
    // IBitmapBuffer
    kBufferPlaneDescription = 7,
    // IMemoryBuffer
    kMemoryCreateReference = 6,
    // IMemoryBufferByteAccess (IUnknown only)
    kBytesGetBuffer = 3,
    // IAsyncInfo
    kAsyncStatus = 7, kAsyncErrorCode = 8,
    // IAsyncOperation<T>
    kOperationGetResults = 8,
    // IVectorView<T>
    kViewGetAt = 6, kViewSize = 7,
    // IReference<T>
    kReferenceValue = 6,
    // IClosable
    kClose = 6,
};
enum { kGray8 = 62, kLockWrite = 2, kAsyncStarted = 0, kAsyncCompleted = 1 };
struct PlaneDescription { INT32 start, width, height, stride; };
struct RectFloat { float x, y, w, h; };

template <class... A> HRESULT Call(void *obj, int slot, A... a)
{
    typedef HRESULT(STDMETHODCALLTYPE * Fn)(void *, A...);
    return ((Fn)(*(void ***)obj)[slot])(obj, a...);
}

void Release(void *obj)
{
    if (obj) ((IUnknown *)obj)->Release();
}

// Releases what it holds when it goes.
struct Obj {
    void *p = nullptr;
    Obj() = default;
    ~Obj() { Release(p); }
    Obj(const Obj &) = delete;
    Obj &operator=(const Obj &) = delete;
    void **Put()
    {
        Release(p);
        p = nullptr;
        return &p;
    }
    explicit operator bool() const { return p != nullptr; }
};

struct Str {
    HStr h = nullptr;
    Str() = default;
    explicit Str(const std::wstring &s) { Runtime().create(s.c_str(), (UINT32)s.size(), &h); }
    ~Str()
    {
        if (h) Runtime().del(h);
    }
    Str(const Str &) = delete;
    Str &operator=(const Str &) = delete;
    std::wstring Text() const
    {
        UINT32 n = 0;
        const wchar_t *p = h ? Runtime().raw(h, &n) : nullptr;
        return p ? std::wstring(p, n) : std::wstring();
    }
};

HRESULT Query(void *obj, const GUID &iid, void **out)
{
    return ((IUnknown *)obj)->QueryInterface(iid, out);
}

void CloseObject(void *obj)
{
    Obj c;
    if (obj && SUCCEEDED(Query(obj, kIClosable, c.Put())) && c) Call(c.p, kClose);
}

// A Gray8 SoftwareBitmap holding the picture.
HRESULT MakeBitmap(void *factory, const uint8_t *gray, int w, int h, int stride, void **bitmap)
{
    Obj bmp, buf, mem, ref, bytes;
    HRESULT hr = Call(factory, kBitmapCreate, (INT32)kGray8, (INT32)w, (INT32)h, bmp.Put());
    if (FAILED(hr) || !bmp) return FAILED(hr) ? hr : E_FAIL;
    hr = Call(bmp.p, kBitmapLockBuffer, (INT32)kLockWrite, buf.Put());
    if (FAILED(hr) || !buf) return FAILED(hr) ? hr : E_FAIL;
    PlaneDescription pd = {};
    hr = Call(buf.p, kBufferPlaneDescription, (INT32)0, &pd);
    if (SUCCEEDED(hr)) hr = Query(buf.p, kIMemoryBuffer, mem.Put());
    if (SUCCEEDED(hr)) hr = Call(mem.p, kMemoryCreateReference, ref.Put());
    if (SUCCEEDED(hr)) hr = Query(ref.p, kIMemoryBufferByteAccess, bytes.Put());
    BYTE *data = nullptr;
    UINT32 cap = 0;
    if (SUCCEEDED(hr)) hr = Call(bytes.p, kBytesGetBuffer, &data, &cap);
    if (SUCCEEDED(hr)) {
        if (!data || pd.width != w || pd.height != h || pd.stride < w || pd.start < 0 ||
            (UINT64)pd.start + (UINT64)pd.stride * (UINT64)(h - 1) + (UINT64)w > cap)
            hr = E_UNEXPECTED;
    }
    if (SUCCEEDED(hr))
        for (int y = 0; y < h; ++y) memcpy(data + pd.start + (size_t)y * pd.stride, gray + (size_t)y * stride, (size_t)w);
    // the lock ends when the reference and the buffer are closed
    bytes.Put();
    CloseObject(ref.p);
    ref.Put();
    mem.Put();
    CloseObject(buf.p);
    buf.Put();
    if (FAILED(hr)) return hr;
    *bitmap = bmp.p;
    bmp.p = nullptr;
    return S_OK;
}

std::wstring Hr(HRESULT hr) { return Fmt(L"0x%08lx", (unsigned long)hr); }

} // namespace

// A language alone ("ja") as the full tag Windows' OCR packs are named by ("ja-JP"): the common ones.
static std::wstring FullTag(const std::wstring &lang)
{
    static const wchar_t *const full[][2] = { { L"ja", L"ja-JP" }, { L"ko", L"ko-KR" }, { L"zh", L"zh-CN" }, { L"en", L"en-US" },
                                              { L"de", L"de-DE" }, { L"fr", L"fr-FR" }, { L"es", L"es-ES" }, { L"ru", L"ru-RU" } };
    for (auto &f : full)
        if (lang == f[0]) return f[1];
    return lang;
}

std::wstring OcrInstallCommand(const std::wstring &lang)
{
    return L"Add-WindowsCapability -Online -Name \"Language.OCR~~~" + FullTag(lang) + L"~0.0.1.0\"";
}

bool Ocr::Open(const std::wstring &lang, std::wstring &error, bool *missing)
{
    Close();
    if (missing) *missing = false;
    const Rt &rt = Runtime();
    if (!rt.ok) {
        error = L"the Windows Runtime isn't available";
        return false;
    }
    if (!inited_) {
        const HRESULT hr = rt.init(1 /* RO_INIT_MULTITHREADED */);
        inited_ = hr == S_OK || hr == S_FALSE;   // (RPC_E_CHANGED_MODE: the thread has COM already, which does)
    }
    Obj statics, languages, language, engine;
    HRESULT hr = rt.factory(Str(L"Windows.Media.Ocr.OcrEngine").h, kIOcrEngineStatics, statics.Put());
    if (SUCCEEDED(hr)) hr = rt.factory(Str(L"Windows.Globalization.Language").h, kILanguageFactory, languages.Put());
    if (SUCCEEDED(hr)) hr = rt.factory(Str(L"Windows.Graphics.Imaging.SoftwareBitmap").h, kISoftwareBitmapFactory, &bitmaps_);
    if (FAILED(hr) || !statics || !languages || !bitmaps_) {
        error = L"Windows' text recognition isn't available (" + Hr(hr) + L")";
        Close();
        return false;
    }
    // "ja" as it is, then as "ja-JP" (whichever Windows' recogniser goes by)
    unsigned char supported = 0;
    for (const std::wstring &tag : { lang, FullTag(lang) }) {
        hr = Call(languages.p, kLanguageCreate, Str(tag).h, language.Put());
        if (FAILED(hr) || !language) {
            error = L"Windows doesn't know the language \"" + lang + L"\" (" + Hr(hr) + L")";
            Close();
            return false;
        }
        supported = 0;
        hr = Call(statics.p, kStaticsIsSupported, language.p, &supported);
        if (FAILED(hr) || supported) break;
    }
    if (SUCCEEDED(hr) && supported) hr = Call(statics.p, kStaticsTryCreate, language.p, engine.Put());
    if (FAILED(hr) || !supported || !engine) {
        if (SUCCEEDED(hr) && missing) *missing = true;
        error = SUCCEEDED(hr) ? L"Windows has no text recognition for \"" + lang + L"\" installed"
                              : L"Windows' text recognition for \"" + lang + L"\" didn't start (" + Hr(hr) + L")";
        Close();
        return false;
    }
    UINT32 dim = 0;
    Call(statics.p, kStaticsMaxDimension, &dim);
    maxDim_ = dim ? (int)dim : 2600;
    engine_ = engine.p;
    engine.p = nullptr;
    return true;
}

void Ocr::Close()
{
    Release(engine_);
    Release(bitmaps_);
    engine_ = bitmaps_ = nullptr;
}

Ocr::~Ocr()
{
    Close();
    if (inited_) Runtime().uninit();   // (only once nothing of the runtime's is held any more)
}

bool Ocr::Recognize(const uint8_t *gray, int w, int h, int stride, OcrPage &out, std::wstring &error)
{
    out = OcrPage();
    if (!engine_ || !bitmaps_) {
        error = L"text recognition isn't open";
        return false;
    }
    if (w < 16 || h < 16 || w > maxDim_ || h > maxDim_) {
        error = Fmt(L"a %d x %d picture can't be read", w, h);
        return false;
    }
    Obj bitmap, op, info, result, lines, angle;
    HRESULT hr = MakeBitmap(bitmaps_, gray, w, h, stride, bitmap.Put());
    if (SUCCEEDED(hr)) hr = Call(engine_, kEngineRecognizeAsync, bitmap.p, op.Put());
    if (SUCCEEDED(hr)) hr = Query(op.p, kIAsyncInfo, info.Put());
    if (SUCCEEDED(hr)) {
        INT32 status = kAsyncStarted;
        const DWORD t0 = GetTickCount();
        for (;;) {
            hr = Call(info.p, kAsyncStatus, &status);
            if (FAILED(hr) || status != kAsyncStarted) break;
            if (GetTickCount() - t0 > 10000) {
                hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
                break;
            }
            Sleep(1);
        }
        if (SUCCEEDED(hr) && status != kAsyncCompleted) {
            HRESULT code = E_FAIL;
            Call(info.p, kAsyncErrorCode, &code);
            hr = FAILED(code) ? code : E_FAIL;
        }
    }
    if (SUCCEEDED(hr)) hr = Call(op.p, kOperationGetResults, result.Put());
    CloseObject(bitmap.p);
    if (SUCCEEDED(hr) && !result) hr = E_FAIL;
    if (SUCCEEDED(hr)) hr = Call(result.p, kResultLines, lines.Put());
    if (FAILED(hr) || !lines) {
        error = L"text recognition failed (" + Hr(FAILED(hr) ? hr : E_FAIL) + L")";
        return false;
    }
    if (SUCCEEDED(Call(result.p, kResultTextAngle, angle.Put())) && angle) {
        double a = 0;
        if (SUCCEEDED(Call(angle.p, kReferenceValue, &a))) out.angle = (float)a;
    }
    UINT32 n = 0;
    Call(lines.p, kViewSize, &n);
    for (UINT32 i = 0; i < n; ++i) {
        Obj line, words;
        if (FAILED(Call(lines.p, kViewGetAt, i, line.Put())) || !line) continue;
        if (FAILED(Call(line.p, kLineWords, words.Put())) || !words) continue;
        UINT32 nw = 0;
        Call(words.p, kViewSize, &nw);
        OcrLine l;
        for (UINT32 k = 0; k < nw; ++k) {
            Obj word;
            if (FAILED(Call(words.p, kViewGetAt, k, word.Put())) || !word) continue;
            RectFloat r = {};
            Str text;
            if (FAILED(Call(word.p, kWordRect, &r)) || FAILED(Call(word.p, kWordText, &text.h))) continue;
            OcrWord wd;
            wd.text = text.Text();
            wd.x = r.x;
            wd.y = r.y;
            wd.w = r.w;
            wd.h = r.h;
            if (!wd.text.empty()) l.words.push_back(wd);
        }
        if (!l.words.empty()) out.lines.push_back(l);
    }
    return true;
}

} // namespace app
