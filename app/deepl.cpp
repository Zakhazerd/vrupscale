// DeepL's API over WinHTTP (see deepl.h).
#include "deepl.h"
#include "core.h"
#include "json.h"

#include <winhttp.h>

namespace app {

namespace {

std::string ToUtf8(const std::wstring &w)
{
    std::string s;
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    s.resize(n > 0 ? n : 0);
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring FromUtf8(const std::string &s)
{
    std::wstring w;
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    w.resize(n > 0 ? n : 0);
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

std::string JsonString(const std::wstring &w)
{
    std::string o = "\"";
    for (unsigned char c : ToUtf8(w)) {
        if (c == '"') o += "\\\"";
        else if (c == '\\') o += "\\\\";
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else if (c == '\t') o += "\\t";
        else if (c < 0x20) {
            char u[8];
            snprintf(u, sizeof(u), "\\u%04x", c);
            o += u;
        } else o += (char)c;
    }
    return o + "\"";
}

// A language tag as DeepL names languages. A source is the language alone ("ja" -> "JA", "en-US" ->
// "EN"). A target keeps its variant where DeepL has variants (English, Portuguese, Chinese: "en-US"
// -> "EN-US"; English alone is taken as "EN-US", Portuguese alone as "PT-PT"); other targets are the
// language alone ("de-DE" -> "DE").
std::string LangCode(const std::wstring &tag, bool target)
{
    std::string s = ToUtf8(tag);
    for (char &c : s) c = (char)toupper((unsigned char)c);
    const size_t dash = s.find('-');
    const std::string lang = s.substr(0, dash);
    if (!target) return lang;
    const bool variants = lang == "EN" || lang == "PT" || lang == "ZH";
    if (dash != std::string::npos) return variants ? s : lang;
    return lang == "EN" ? "EN-US" : (lang == "PT" ? "PT-PT" : lang);
}

std::string RequestBody(const std::vector<std::wstring> &texts, const std::wstring &source, const std::wstring &target)
{
    std::string b = "{\"text\":[";
    for (size_t i = 0; i < texts.size(); ++i) b += (i ? "," : "") + JsonString(texts[i]);
    b += "],\"source_lang\":\"" + LangCode(source, false) + "\",\"target_lang\":\"" + LangCode(target, true) + "\"}";
    return b;
}

// The translations in an answer, in order. False if it isn't one.
bool ParseTranslations(const std::string &json, std::vector<std::wstring> &out)
{
    JVal root;
    if (!Parse(json, root) || root.kind != JVal::Obj) return false;
    JVal *tr = root.Find("translations");
    if (!tr || tr->kind != JVal::Arr) return false;
    for (JVal &t : tr->items) {
        JVal *text = t.kind == JVal::Obj ? t.Find("text") : nullptr;
        if (!text || text->kind != JVal::Str) return false;
        out.push_back(FromUtf8(Unescape(text->raw)));
    }
    return true;
}

// DeepL's own words for an error ({"message": "..."}), if the answer has them.
std::wstring ErrorMessage(const std::string &json)
{
    JVal root;
    if (!Parse(json, root) || root.kind != JVal::Obj) return std::wstring();
    JVal *m = root.Find("message");
    return m && m->kind == JVal::Str ? FromUtf8(Unescape(m->raw)) : std::wstring();
}

struct Inet {
    HINTERNET h;
    explicit Inet(HINTERNET x) : h(x) {}
    ~Inet()
    {
        if (h) WinHttpCloseHandle(h);
    }
    Inet(const Inet &) = delete;
    Inet &operator=(const Inet &) = delete;
};

// The server: DeepL's for this key, or the test's on this computer.
std::wstring Server(const std::wstring &key)
{
    wchar_t buf[256];
    const DWORD n = GetEnvironmentVariableW(L"VRUPSCALE_DEEPL_URL", buf, 256);
    if (n > 0 && n < 256) {
        const std::wstring u(buf, n);
        if (u.rfind(L"http://127.0.0.1:", 0) == 0 || u.rfind(L"http://localhost:", 0) == 0) return u;
    }
    const bool free = key.size() > 3 && key.compare(key.size() - 3, 3, L":fx") == 0;
    return free ? L"https://api-free.deepl.com" : L"https://api.deepl.com";
}

// One request with the key. body empty: a GET. False: no answer (err says why).
bool Request(const std::wstring &key, const wchar_t *path, const std::string &body, std::string &out, int &status, std::wstring &err)
{
    status = 0;
    out.clear();
    const std::wstring url = Server(key) + path;
    URL_COMPONENTS uc = {};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256], target[512];
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = target;
    uc.dwUrlPathLength = 512;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) {
        err = L"the address isn't usable";
        return false;
    }
    Inet session(WinHttpOpen(L"vrupscale", 4 /* WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY */, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.h) {
        err = Fmt(L"no connection (%lu)", GetLastError());
        return false;
    }
    WinHttpSetTimeouts(session.h, 4000, 4000, 6000, 10000);
    Inet conn(WinHttpConnect(session.h, host, uc.nPort, 0));
    Inet req(conn.h ? WinHttpOpenRequest(conn.h, body.empty() ? L"GET" : L"POST", target, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                            : nullptr);
    if (!req.h) {
        err = Fmt(L"no connection (%lu)", GetLastError());
        return false;
    }
    const std::wstring headers = L"Authorization: DeepL-Auth-Key " + key + L"\r\nContent-Type: application/json\r\n";
    if (!WinHttpSendRequest(req.h, headers.c_str(), (DWORD)-1L, body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(), (DWORD)body.size(),
                            (DWORD)body.size(), 0) ||
        !WinHttpReceiveResponse(req.h, nullptr)) {
        const DWORD e = GetLastError();
        err = e == ERROR_WINHTTP_TIMEOUT ? L"it didn't answer in time"
              : (e == ERROR_WINHTTP_NAME_NOT_RESOLVED || e == ERROR_WINHTTP_CANNOT_CONNECT) ? L"it can't be reached (no internet?)"
                                                                                         : Fmt(L"the request failed (%lu)", e);
        return false;
    }
    DWORD code = 0, sz = sizeof(code);
    WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &sz,
                        WINHTTP_NO_HEADER_INDEX);
    status = (int)code;
    for (;;) {
        DWORD avail = 0, got = 0;
        if (!WinHttpQueryDataAvailable(req.h, &avail) || avail == 0) break;
        const size_t at = out.size();
        if (at + avail > (4u << 20)) break;   // (an answer is a few kilobytes)
        out.resize(at + avail);
        if (!WinHttpReadData(req.h, &out[at], avail, &got)) {
            out.resize(at);
            break;
        }
        out.resize(at + got);
    }
    return true;
}

std::wstring StatusText(int status, const std::string &answer)
{
    if (status == 403) return L"DeepL refused the key";
    if (status == 456) return L"this month's DeepL characters are used up";
    if (status == 429) return L"DeepL is asked too often right now";
    const std::wstring m = ErrorMessage(answer);
    return Fmt(L"DeepL answered %d", status) + (m.empty() ? L"" : L" (" + m + L")");
}

} // namespace

bool DeepLKeyPlausible(const std::wstring &key)
{
    if (key.size() < 20 || key.size() > 80) return false;
    for (wchar_t c : key)
        if (!(iswalnum(c) || c == L'-' || c == L':') || c > 127) return false;
    return true;
}

std::wstring DeepLServer(const std::wstring &key)
{
    const std::wstring s = Server(key);
    const size_t at = s.find(L"//");
    return at == std::wstring::npos ? s : s.substr(at + 2);
}

DeepLResult DeepLTranslate(const std::wstring &key, const std::vector<std::wstring> &texts, const std::wstring &source,
                           const std::wstring &target)
{
    DeepLResult r;
    if (!DeepLKeyPlausible(key)) {
        r.error = L"no DeepL key is set";
        r.fatal = true;
        return r;
    }
    if (texts.empty()) {
        r.ok = true;
        return r;
    }
    std::string answer;
    if (!Request(key, L"/v2/translate", RequestBody(texts, source, target), answer, r.status, r.error)) {
        r.error = L"DeepL: " + r.error;
        return r;
    }
    if (r.status != 200) {
        r.error = StatusText(r.status, answer);
        r.fatal = r.status == 403 || r.status == 456;
        return r;
    }
    if (!ParseTranslations(answer, r.texts) || r.texts.size() != texts.size()) {
        r.texts.clear();
        r.error = L"DeepL's answer couldn't be read";
        return r;
    }
    r.ok = true;
    return r;
}

DeepLUsage DeepLGetUsage(const std::wstring &key)
{
    DeepLUsage u;
    if (!DeepLKeyPlausible(key)) {
        u.error = L"no DeepL key is set";
        return u;
    }
    std::string answer;
    int status = 0;
    if (!Request(key, L"/v2/usage", std::string(), answer, status, u.error)) {
        u.error = L"DeepL: " + u.error;
        return u;
    }
    if (status != 200) {
        u.error = StatusText(status, answer);
        return u;
    }
    JVal root;
    JVal *c = Parse(answer, root) && root.kind == JVal::Obj ? root.Find("character_count") : nullptr;
    JVal *l = c ? root.Find("character_limit") : nullptr;
    if (!c || !l) {
        u.error = L"DeepL's answer couldn't be read";
        return u;
    }
    u.count = strtoull(c->raw.c_str(), nullptr, 10);
    u.limit = strtoull(l->raw.c_str(), nullptr, 10);
    u.ok = true;
    return u;
}

int DeepLSelfTest(std::wstring &report)
{
    int fails = 0;
    auto check = [&](bool ok, const wchar_t *what) {
        report += Fmt(L"%s %s\n", ok ? L"ok  " : L"FAIL", what);
        if (!ok) ++fails;
    };
    // "a \"b\" \\ <newline> c", then Japanese
    const std::vector<std::wstring> texts = { L"a \"b\" \\ \n c", L"\x3053\x3093\x306b\x3061\x306f" };
    const std::string body = RequestBody(texts, L"ja", L"en-US");
    check(body == "{\"text\":[\"a \\\"b\\\" \\\\ \\n c\",\"\xE3\x81\x93\xE3\x82\x93\xE3\x81\xAB\xE3\x81\xA1\xE3\x81\xAF\"],"
                  "\"source_lang\":\"JA\",\"target_lang\":\"EN-US\"}",
          L"deepl: the request holds the texts (escaped, UTF-8) and the languages");
    JVal echo;
    check(Parse(body, echo) && echo.Find("text") && echo.Find("text")->items.size() == 2 &&
              FromUtf8(Unescape(echo.Find("text")->items[0].raw)) == texts[0],
          L"deepl: the request is JSON that reads back as the texts");
    std::vector<std::wstring> out;
    check(ParseTranslations("{\"translations\":[{\"detected_source_language\":\"JA\",\"text\":\"Hello \\\"you\\\"\"},"
                            "{\"detected_source_language\":\"JA\",\"text\":\"caf\\u00e9 \xE2\x86\x92\"}]}", out) &&
              out.size() == 2 && out[0] == L"Hello \"you\"" && out[1] == L"caf\x00e9 \x2192",
          L"deepl: an answer's translations are read in order (escapes, UTF-8)");
    out.clear();
    check(!ParseTranslations("{\"message\":\"Quota exceeded\"}", out) && ErrorMessage("{\"message\":\"Quota exceeded\"}") == L"Quota exceeded",
          L"deepl: an error answer is not taken for translations, and its message is kept");
    check(DeepLKeyPlausible(L"12345678-abcd-1234-abcd-1234567890ab:fx") && !DeepLKeyPlausible(L"") && !DeepLKeyPlausible(L"hello world, this is not a key"),
          L"deepl: a key's shape is checked");
    check(LangCode(L"ja", false) == "JA" && LangCode(L"en-US", false) == "EN" && LangCode(L"zh-CN", false) == "ZH" && LangCode(L"en-US", true) == "EN-US" &&
              LangCode(L"en", true) == "EN-US" && LangCode(L"en-GB", true) == "EN-GB" && LangCode(L"pt-BR", true) == "PT-BR" && LangCode(L"de-DE", true) == "DE" &&
              LangCode(L"ja", true) == "JA",
          L"deepl: languages are named as DeepL names them (a source without its region, a target with its variant where there are variants)");
    wchar_t env[8];
    if (!GetEnvironmentVariableW(L"VRUPSCALE_DEEPL_URL", env, 8))
        check(DeepLServer(L"12345678-abcd-1234-abcd-1234567890ab:fx") == L"api-free.deepl.com" &&
                  DeepLServer(L"12345678-abcd-1234-abcd-1234567890ab") == L"api.deepl.com",
              L"deepl: free keys go to api-free.deepl.com, others to api.deepl.com");
    DeepLResult none = DeepLTranslate(L"", texts, L"ja", L"en-US");
    check(!none.ok && none.fatal && none.status == 0, L"deepl: without a key nothing is sent");
    return fails;
}

} // namespace app
