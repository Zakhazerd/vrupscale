// Updates from GitHub releases (see update.h).
#include "update.h"
#include "core.h"
#include "json.h"

#include <bcrypt.h>
#include <winhttp.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>

namespace app {

namespace {

constexpr uint64_t kMaxZip = 64ull << 20;   // a release is a few MB

std::wstring EnvOr(const wchar_t *name, const std::wstring &fallback)
{
    wchar_t buf[1024];
    DWORD n = GetEnvironmentVariableW(name, buf, 1024);
    return (n > 0 && n < 1024) ? std::wstring(buf, n) : fallback;
}

std::wstring FromUtf8(const std::string &s)
{
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

bool FileExistsW(const std::wstring &p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

// ---- HTTPS GET ------------------------------------------------------------------------------------

struct Inet {
    HINTERNET h = nullptr;
    explicit Inet(HINTERNET x) : h(x) {}
    ~Inet()
    {
        if (h) WinHttpCloseHandle(h);
    }
};

// Plain http only for a local test server (VRUPSCALE_UPDATE_URL); everything else must be https.
bool AllowedUrl(const std::wstring &url)
{
    if (url.compare(0, 8, L"https://") == 0) return true;
    return url.compare(0, 17, L"http://127.0.0.1:") == 0 || url.compare(0, 17, L"http://localhost:") == 0;
}

typedef void (*ProgressFn)(uint64_t, uint64_t, void *);

bool HttpGet(const std::wstring &url, bool api, std::string &out, int &status, std::wstring &err, ProgressFn progress = nullptr,
             void *ctx = nullptr, volatile long *cancel = nullptr)
{
    out.clear();
    status = 0;
    if (!AllowedUrl(url)) {
        err = L"refused a non-https address";
        return false;
    }
    URL_COMPONENTS uc = { sizeof(uc) };
    wchar_t host[256] = L"", path[2048] = L"", extra[1024] = L"";
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2048;
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = 1024;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) {
        err = L"bad address";
        return false;
    }
    const std::wstring ua = Widen("vrupscale/" VU_APP_VERSION " (+https://github.com/Zakhazerd/vrupscale)");
    // Windows 8.1+: the system's proxy settings, including automatic ones; else the WinHTTP proxy
    Inet session(WinHttpOpen(ua.c_str(), 4 /* WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY */, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.h)
        session.h = WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.h) {
        err = Fmt(L"no internet access (error %lu)", GetLastError());
        return false;
    }
    WinHttpSetTimeouts(session.h, 10000, 10000, 15000, 30000);
    Inet conn(WinHttpConnect(session.h, host, uc.nPort, 0));
    const bool secure = uc.nScheme == INTERNET_SCHEME_HTTPS;
    const std::wstring target = std::wstring(path) + extra;
    Inet req(conn.h ? WinHttpOpenRequest(conn.h, L"GET", target.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         secure ? WINHTTP_FLAG_SECURE : 0)
                    : nullptr);
    if (!req.h) {
        err = Fmt(L"could not reach %s (error %lu)", host, GetLastError());
        return false;
    }
    const wchar_t *headers = api ? L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n"
                                 : L"Accept: application/octet-stream\r\n";
    if (!WinHttpSendRequest(req.h, headers, (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(req.h, nullptr)) {
        const DWORD e = GetLastError();
        err = e == ERROR_WINHTTP_NAME_NOT_RESOLVED || e == ERROR_WINHTTP_CANNOT_CONNECT ? std::wstring(L"no connection to GitHub")
              : e == ERROR_WINHTTP_TIMEOUT ? std::wstring(L"GitHub did not answer in time")
                                           : Fmt(L"the request to %s failed (error %lu)", host, e);
        return false;
    }
    DWORD code = 0, sz = sizeof(code);
    WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &sz,
                        WINHTTP_NO_HEADER_INDEX);
    status = (int)code;
    wchar_t lenText[32] = L"";
    sz = sizeof(lenText);
    uint64_t total = 0;
    if (WinHttpQueryHeaders(req.h, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, lenText, &sz, WINHTTP_NO_HEADER_INDEX))
        total = _wcstoui64(lenText, nullptr, 10);
    if (total > kMaxZip) {
        err = L"the download is far larger than a release should be";
        return false;
    }
    if (total) out.reserve((size_t)total);
    for (;;) {
        if (cancel && *cancel) {
            err = L"cancelled";
            return false;
        }
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req.h, &avail)) {
            err = Fmt(L"the download broke off (error %lu)", GetLastError());
            return false;
        }
        if (!avail) break;
        const size_t at = out.size();
        out.resize(at + avail);
        DWORD got = 0;
        if (!WinHttpReadData(req.h, &out[at], avail, &got)) {
            err = Fmt(L"the download broke off (error %lu)", GetLastError());
            return false;
        }
        out.resize(at + got);
        if (out.size() > kMaxZip) {
            err = L"the download is far larger than a release should be";
            return false;
        }
        if (progress) progress(out.size(), total, ctx);
    }
    return true;
}

// ---- SHA-256 and CRC-32 -----------------------------------------------------------------------------

std::string Sha256Hex(const void *data, size_t n)
{
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    unsigned char d[32];
    std::string hex;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return hex;
    if (BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0) {
        const unsigned char *p = (const unsigned char *)data;
        bool ok = true;
        while (n > 0 && ok) {   // in pieces: BCryptHashData takes a ULONG
            const ULONG part = (ULONG)std::min<size_t>(n, 1u << 24);
            ok = BCryptHashData(h, (PUCHAR)p, part, 0) == 0;
            p += part;
            n -= part;
        }
        if (ok && BCryptFinishHash(h, d, 32, 0) == 0) {
            static const char *x = "0123456789abcdef";
            for (unsigned char b : d) {
                hex += x[b >> 4];
                hex += x[b & 15];
            }
        }
        BCryptDestroyHash(h);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return hex;
}

uint32_t Crc32(const unsigned char *p, size_t n)
{
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// The first 64 hex digits of a sha256sum line ("<hex>  <name>"), lower case; "" if none.
std::string HexDigest(const std::string &s)
{
    std::string h;
    for (char c : s) {
        if (isxdigit((unsigned char)c)) h += (char)tolower((unsigned char)c);
        else if (!h.empty()) break;
        if (h.size() == 64) break;
    }
    return h.size() == 64 ? h : std::string();
}

// ---- the release zip (stored entries only) ---------------------------------------------------------

uint16_t U16(const unsigned char *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t U32(const unsigned char *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

struct ZipFile {
    std::string rel;            // path inside the release folder, '/'-separated
    size_t offset = 0, size = 0;
};

// A path the release may contain: relative, no "..", plain characters.
bool SafeRel(const std::string &r)
{
    if (r.empty() || r[0] == '/' || r.find("..") != std::string::npos || r.find(':') != std::string::npos) return false;
    for (char c : r)
        if (!(isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-' || c == '/' || c == ' ')) return false;
    return true;
}

// Lists the files of a zip whose entries all sit in one top folder (vrupscale-<version>/), checking
// each one's CRC. False + error if it is not such a zip.
bool ReadZip(const std::string &z, std::vector<ZipFile> &files, std::string &top, std::wstring &err)
{
    const unsigned char *b = (const unsigned char *)z.data();
    const size_t n = z.size();
    files.clear();
    top.clear();
    if (n < 22) {
        err = L"the download is not a zip";
        return false;
    }
    size_t eocd = std::string::npos;
    for (size_t i = n - 22 + 1; i-- > 0 && n - i <= 22 + 65535;)
        if (U32(b + i) == 0x06054b50u) {
            eocd = i;
            break;
        }
    if (eocd == std::string::npos) {
        err = L"the download is not a zip";
        return false;
    }
    const uint16_t count = U16(b + eocd + 10);
    size_t cd = U32(b + eocd + 16);
    for (uint16_t k = 0; k < count; ++k) {
        if (cd + 46 > n || U32(b + cd) != 0x02014b50u) {
            err = L"the zip's directory is damaged";
            return false;
        }
        const uint16_t flags = U16(b + cd + 8), method = U16(b + cd + 10);
        const uint32_t crc = U32(b + cd + 16), csize = U32(b + cd + 20), usize = U32(b + cd + 24);
        const uint16_t nameLen = U16(b + cd + 28), extraLen = U16(b + cd + 30), commentLen = U16(b + cd + 32);
        const uint32_t local = U32(b + cd + 42);
        if (cd + 46 + nameLen > n) {
            err = L"the zip's directory is damaged";
            return false;
        }
        std::string name((const char *)b + cd + 46, nameLen);
        cd += 46 + nameLen + extraLen + commentLen;
        for (char &c : name)
            if (c == '\\') c = '/';
        const size_t slash = name.find('/');
        if (slash == std::string::npos || slash == 0) {
            err = L"the zip is not a vrupscale release (files outside its folder)";
            return false;
        }
        const std::string t = name.substr(0, slash), rel = name.substr(slash + 1);
        if (top.empty()) top = t;
        if (t != top) {
            err = L"the zip is not a vrupscale release (more than one folder)";
            return false;
        }
        if (rel.empty() || rel.back() == '/') continue;   // a folder
        if (!SafeRel(rel)) {
            err = L"the zip holds a file with an unexpected name";
            return false;
        }
        if (flags & 1) {
            err = L"the zip is encrypted";
            return false;
        }
        if (method != 0 || csize != usize) {
            err = L"the zip is compressed (a vrupscale release is stored uncompressed for the updater)";
            return false;
        }
        if ((size_t)local + 30 > n || U32(b + local) != 0x04034b50u) {
            err = L"the zip is damaged";
            return false;
        }
        const size_t data = (size_t)local + 30 + U16(b + local + 26) + U16(b + local + 28);
        if (data + usize > n) {
            err = L"the zip is damaged (cut short?)";
            return false;
        }
        if (Crc32(b + data, usize) != crc) {
            err = L"a file in the zip is damaged (CRC mismatch)";
            return false;
        }
        files.push_back({ rel, data, usize });
    }
    if (files.empty()) {
        err = L"the zip is empty";
        return false;
    }
    return true;
}

bool MakeDirs(const std::wstring &dir)
{
    if (dir.empty() || FileExistsW(dir)) return true;
    const size_t s = dir.find_last_of(L"\\/");
    if (s != std::wstring::npos && s > 2 && !MakeDirs(dir.substr(0, s))) return false;
    return CreateDirectoryW(dir.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

bool WriteAllBytes(const std::wstring &path, const void *data, size_t n)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    const bool ok = n == 0 || (WriteFile(f, data, (DWORD)n, &wr, nullptr) && wr == n);
    CloseHandle(f);
    return ok;
}

void DeleteTree(const std::wstring &dir)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            const std::wstring p = dir + L"\\" + name;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) DeleteTree(p);
            else {
                SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(p.c_str());
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

void ListFiles(const std::wstring &base, const std::wstring &rel, std::vector<std::wstring> &out)
{
    WIN32_FIND_DATAW fd;
    const std::wstring dir = rel.empty() ? base : base + L"\\" + rel;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        const std::wstring r = rel.empty() ? name : rel + L"\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ListFiles(base, r, out);
        else out.push_back(r);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

std::wstring UpdateDir() { return DataDir() + L"\\update"; }
std::wstring CleanupList() { return UpdateDir() + L"\\cleanup.txt"; }

} // namespace

// ---- public ---------------------------------------------------------------------------------------

int CompareVersions(const std::wstring &a, const std::wstring &b)
{
    size_t i = (!a.empty() && (a[0] == L'v' || a[0] == L'V')) ? 1 : 0;
    size_t j = (!b.empty() && (b[0] == L'v' || b[0] == L'V')) ? 1 : 0;
    for (int part = 0; part < 4; ++part) {
        long x = 0, y = 0;
        while (i < a.size() && iswdigit(a[i])) x = x * 10 + (a[i++] - L'0');
        while (j < b.size() && iswdigit(b[j])) y = y * 10 + (b[j++] - L'0');
        if (x != y) return x < y ? -1 : 1;
        if (i < a.size() && a[i] == L'.') ++i;
        if (j < b.size() && b[j] == L'.') ++j;
    }
    return 0;
}

UpdateInfo CheckForUpdate(const std::wstring &currentVersion)
{
    UpdateInfo u;
    const std::wstring url = EnvOr(L"VRUPSCALE_UPDATE_URL", std::wstring(L"https://api.github.com/repos/") + kUpdateRepo + L"/releases/latest");
    std::string body;
    int status = 0;
    if (!HttpGet(url, true, body, status, u.error)) return u;
    if (status == 404) {   // no release published yet
        u.ok = true;
        return u;
    }
    if (status == 403 || status == 429) {
        u.error = L"GitHub asks to wait a while (rate limit)";
        return u;
    }
    if (status != 200) {
        u.error = Fmt(L"GitHub answered %d", status);
        return u;
    }
    JVal root;
    if (!Parse(body, root) || root.kind != JVal::Obj) {
        u.error = L"GitHub's answer could not be read";
        return u;
    }
    auto str = [&](JVal *v) { return v && v->kind == JVal::Str ? FromUtf8(Unescape(v->raw)) : std::wstring(); };
    std::wstring tag = str(root.Find("tag_name"));
    if (!tag.empty() && (tag[0] == L'v' || tag[0] == L'V')) tag = tag.substr(1);
    if (tag.empty() || !iswdigit(tag[0])) {
        u.error = L"the latest release has no version tag";
        return u;
    }
    JVal *draft = root.Find("draft"), *pre = root.Find("prerelease");
    if ((draft && draft->raw == "true") || (pre && pre->raw == "true")) {   // "latest" never is; to be safe
        u.ok = true;
        return u;
    }
    u.version = tag;
    u.title = str(root.Find("name"));
    u.notes = str(root.Find("body"));
    u.page = str(root.Find("html_url"));
    u.zipName = L"vrupscale-" + tag + L".zip";
    if (JVal *assets = root.Find("assets"))
        for (auto &a : assets->items) {
            if (a.kind != JVal::Obj) continue;
            const std::wstring name = str(a.Find("name")), link = str(a.Find("browser_download_url"));
            if (name == u.zipName) {
                u.zipUrl = link;
                if (JVal *s = a.Find("size")) u.zipSize = _strtoui64(s->raw.c_str(), nullptr, 10);
                const std::wstring dg = str(a.Find("digest"));   // "sha256:<hex>" (GitHub, since 2025)
                if (dg.compare(0, 7, L"sha256:") == 0) {
                    std::string h;
                    for (wchar_t c : dg.substr(7)) h += (char)c;
                    u.sha256 = HexDigest(h);
                }
            } else if (name == u.zipName + L".sha256") {
                u.shaUrl = link;
            }
        }
    u.ok = true;
    u.newer = CompareVersions(tag, currentVersion) > 0;
    if (u.newer && u.zipUrl.empty()) {
        u.newer = false;
        u.error = L"the latest release has no " + u.zipName;
    }
    return u;
}

StagedUpdate DownloadUpdate(const UpdateInfo &u, void (*progress)(uint64_t, uint64_t, void *), void *ctx, volatile long *cancel)
{
    StagedUpdate r;
    r.version = u.version;
    std::string want = u.sha256;
    int status = 0;
    if (want.empty() && !u.shaUrl.empty()) {
        std::string text;
        if (HttpGet(u.shaUrl, false, text, status, r.error, nullptr, nullptr, cancel) && status == 200) want = HexDigest(text);
    }
    if (want.empty()) {
        r.error = L"the release publishes no SHA-256 checksum to check the download against";
        return r;
    }
    std::string zip;
    if (!HttpGet(u.zipUrl, false, zip, status, r.error, progress, ctx, cancel)) return r;
    if (status != 200) {
        r.error = Fmt(L"the download failed (HTTP %d)", status);
        return r;
    }
    if (u.zipSize && zip.size() != u.zipSize) {
        r.error = L"the download is incomplete";
        return r;
    }
    if (Sha256Hex(zip.data(), zip.size()) != want) {
        r.error = L"the download does not match the release's checksum";
        return r;
    }
    std::vector<ZipFile> files;
    std::string top;
    if (!ReadZip(zip, files, top, r.error)) return r;
    bool exe = false, payload = false;
    for (auto &f : files) {
        exe = exe || f.rel == "vrupscale.exe";
        payload = payload || f.rel == "payload/dxgi.dll";
    }
    if (!exe || !payload) {
        r.error = L"the zip is not a vrupscale release (no vrupscale.exe or payload\\dxgi.dll)";
        return r;
    }
    r.dir = UpdateDir() + L"\\" + u.version;
    DeleteTree(r.dir);
    for (auto &f : files) {
        std::wstring rel = FromUtf8(f.rel);
        std::replace(rel.begin(), rel.end(), L'/', L'\\');
        const std::wstring path = r.dir + L"\\" + rel;
        if (!MakeDirs(path.substr(0, path.find_last_of(L'\\'))) || !WriteAllBytes(path, zip.data() + f.offset, f.size)) {
            r.error = L"could not unpack the update into " + r.dir;
            return r;
        }
    }
    // what it brings must be the version it says it is
    const DllInfo d = GetDllInfo(r.dir + L"\\payload\\dxgi.dll");
    if (!d.ours || CompareVersions(d.version, u.version) != 0) {
        r.error = Fmt(L"the release's payload\\dxgi.dll is %s, not vrupscale %s", d.ours ? d.version.c_str() : L"something else",
                      u.version.c_str());
        return r;
    }
    r.ok = true;
    return r;
}

bool ApplyUpdate(const std::wstring &staged, std::wstring &error)
{
    const std::wstring base = ExeDir();
    std::vector<std::wstring> files;
    ListFiles(staged, L"", files);
    if (files.empty()) {
        error = L"the update is empty";
        return false;
    }
    {   // can the app's folder be written at all?
        const std::wstring probe = base + L"\\.vrupscale-update-check";
        if (!WriteAllBytes(probe, "", 0)) {
            error = L"the app's folder can't be written (" + base + L"). Extract the new zip by hand, or move vrupscale to a folder of your own (Documents, say).";
            return false;
        }
        DeleteFileW(probe.c_str());
    }
    struct Done { std::wstring dst; bool moved; };
    std::vector<Done> done;
    auto rollback = [&]() {
        for (auto it = done.rbegin(); it != done.rend(); ++it) {
            DeleteFileW(it->dst.c_str());
            if (it->moved) MoveFileExW((it->dst + L".old").c_str(), it->dst.c_str(), MOVEFILE_REPLACE_EXISTING);
        }
    };
    std::wstring cleanup;
    for (auto &rel : files) {
        const std::wstring src = staged + L"\\" + rel, dst = base + L"\\" + rel;
        if (!MakeDirs(dst.substr(0, dst.find_last_of(L'\\')))) {
            error = L"could not create a folder for " + dst;
            rollback();
            return false;
        }
        bool moved = false;
        if (FileExistsW(dst)) {
            // in use (the running exe, openvr_api.dll) or not: renamed aside, so it can be put back
            SetFileAttributesW(dst.c_str(), FILE_ATTRIBUTE_NORMAL);
            if (!MoveFileExW(dst.c_str(), (dst + L".old").c_str(), MOVEFILE_REPLACE_EXISTING)) {
                error = Fmt(L"could not replace %s (error %lu)", dst.c_str(), GetLastError());
                rollback();
                return false;
            }
            moved = true;
            cleanup += dst + L".old\n";
        }
        done.push_back({ dst, moved });
        if (!CopyFileW(src.c_str(), dst.c_str(), FALSE)) {
            error = Fmt(L"could not write %s (error %lu)", dst.c_str(), GetLastError());
            rollback();
            return false;
        }
    }
    // the renamed files go at the next start (the exe can't be deleted while it runs)
    std::string old;
    {
        HANDLE f = CreateFileW(CleanupList().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            char buf[4096];
            DWORD rd = 0;
            while (ReadFile(f, buf, sizeof(buf), &rd, nullptr) && rd) old.append(buf, rd);
            CloseHandle(f);
        }
    }
    std::string u8;
    const int n = WideCharToMultiByte(CP_UTF8, 0, cleanup.c_str(), (int)cleanup.size(), nullptr, 0, nullptr, nullptr);
    u8.resize(n);
    WideCharToMultiByte(CP_UTF8, 0, cleanup.c_str(), (int)cleanup.size(), &u8[0], n, nullptr, nullptr);
    MakeDirs(UpdateDir());
    old += u8;
    WriteAllBytes(CleanupList(), old.data(), old.size());
    return true;
}

void CleanupAfterUpdate()
{
    std::string text;
    HANDLE f = CreateFileW(CleanupList().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        char buf[4096];
        DWORD rd = 0;
        while (ReadFile(f, buf, sizeof(buf), &rd, nullptr) && rd) text.append(buf, rd);
        CloseHandle(f);
    }
    std::string keep;
    size_t a = 0;
    while (a < text.size()) {
        size_t e = text.find('\n', a);
        if (e == std::string::npos) e = text.size();
        const std::string line = text.substr(a, e - a);
        a = e + 1;
        if (line.size() < 5 || line.compare(line.size() - 4, 4, ".old") != 0) continue;   // only what an update renamed
        const std::wstring p = FromUtf8(line);
        SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
        if (!DeleteFileW(p.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) keep += line + "\n";   // still in use: next time
    }
    if (keep.empty()) DeleteFileW(CleanupList().c_str());
    else WriteAllBytes(CleanupList(), keep.data(), keep.size());
    // unpacked releases that were applied (or abandoned)
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((UpdateDir() + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = fd.cFileName;
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && name != L"." && name != L"..") DeleteTree(UpdateDir() + L"\\" + name);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
}

// ---- self-test ------------------------------------------------------------------------------------

namespace {

void Put16(std::string &s, uint16_t v) { s += (char)(v & 0xFF); s += (char)(v >> 8); }
void Put32(std::string &s, uint32_t v) { for (int i = 0; i < 4; ++i) s += (char)((v >> (8 * i)) & 0xFF); }

// A stored zip, as Python's zipfile writes one.
std::string TestZip(const std::vector<std::pair<std::string, std::string>> &files, uint16_t method = 0)
{
    std::string z, cd;
    for (auto &f : files) {
        const uint32_t off = (uint32_t)z.size(), crc = Crc32((const unsigned char *)f.second.data(), f.second.size());
        Put32(z, 0x04034b50u); Put16(z, 20); Put16(z, 0); Put16(z, method); Put32(z, 0); Put32(z, crc);
        Put32(z, (uint32_t)f.second.size()); Put32(z, (uint32_t)f.second.size()); Put16(z, (uint16_t)f.first.size()); Put16(z, 0);
        z += f.first;
        z += f.second;
        Put32(cd, 0x02014b50u); Put16(cd, 20); Put16(cd, 20); Put16(cd, 0); Put16(cd, method); Put32(cd, 0); Put32(cd, crc);
        Put32(cd, (uint32_t)f.second.size()); Put32(cd, (uint32_t)f.second.size()); Put16(cd, (uint16_t)f.first.size());
        Put16(cd, 0); Put16(cd, 0); Put16(cd, 0); Put16(cd, 0); Put32(cd, 0); Put32(cd, off);
        cd += f.first;
    }
    const uint32_t cdOff = (uint32_t)z.size();
    z += cd;
    Put32(z, 0x06054b50u); Put16(z, 0); Put16(z, 0); Put16(z, (uint16_t)files.size()); Put16(z, (uint16_t)files.size());
    Put32(z, (uint32_t)cd.size()); Put32(z, cdOff); Put16(z, 0);
    return z;
}

} // namespace

int UpdateSelfTest(std::wstring &report)
{
    int fails = 0;
    auto check = [&](bool ok, const wchar_t *what) {
        report += Fmt(L"%s %s\n", ok ? L"ok  " : L"FAIL", what);
        if (!ok) ++fails;
    };
    check(CompareVersions(L"0.6.1", L"0.6.0") > 0 && CompareVersions(L"0.6.10", L"0.6.9") > 0 && CompareVersions(L"v0.6.0", L"0.6.0") == 0 &&
              CompareVersions(L"0.5.9", L"0.6") < 0 && CompareVersions(L"1.0", L"0.9.9") > 0,
          L"versions compare by number (0.6.10 > 0.6.9, v-prefix ignored)");
    check(Sha256Hex("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", L"SHA-256 of \"abc\"");
    check(HexDigest("BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD  vrupscale-0.6.0.zip\n") ==
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          L"a sha256sum line is read");
    std::vector<ZipFile> files;
    std::string top;
    std::wstring err;
    const std::string good = TestZip({ { "vrupscale-9.9.9/", "" }, { "vrupscale-9.9.9/vrupscale.exe", "EXE" },
                                       { "vrupscale-9.9.9/payload/dxgi.dll", "DLL!" } });
    check(ReadZip(good, files, top, err) && top == "vrupscale-9.9.9" && files.size() == 2 && files[0].rel == "vrupscale.exe" &&
              good.substr(files[1].offset, files[1].size) == "DLL!",
          L"a stored release zip is read");
    std::string bad = good;
    bad[bad.find("DLL!")] = 'X';
    check(!ReadZip(bad, files, top, err), L"a damaged file is caught by its CRC");
    check(!ReadZip(TestZip({ { "vrupscale-9.9.9/../evil.exe", "x" } }), files, top, err), L"a path out of the folder is refused");
    check(!ReadZip(TestZip({ { "a/x", "1" }, { "b/y", "2" } }), files, top, err), L"two top folders are refused");
    check(!ReadZip(TestZip({ { "vrupscale-9.9.9/x", "1" } }, 8), files, top, err), L"a compressed entry is refused");
    check(!ReadZip(good.substr(0, good.size() - 30), files, top, err), L"a cut-off zip is refused");
    JVal j;
    check(Parse("{\"body\":\"caf\\u00e9 \\ud83d\\ude00\"}", j) && Unescape(j.Find("body")->raw) == "caf\xc3\xa9 \xf0\x9f\x98\x80",
          L"JSON escapes, including a surrogate pair");
    return fails;
}

} // namespace app
