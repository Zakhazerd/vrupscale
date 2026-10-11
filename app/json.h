// A small JSON tree for the app: SteamVR's settings file (gameres.cpp, which changes one value and
// keeps the rest) and GitHub's release information (update.cpp). Strings and numbers keep their
// source text (escapes included), so nothing is re-encoded on writing.
#pragma once

#include <ctype.h>
#include <stdlib.h>
#include <string>
#include <utility>
#include <vector>

namespace app {

struct JVal {
    enum Kind { Obj, Arr, Str, Num, Lit } kind = Lit;
    std::string raw;                                       // Str (no quotes), Num, Lit
    std::vector<std::pair<std::string, JVal>> members;     // Obj: raw keys, in file order
    std::vector<JVal> items;                               // Arr

    JVal *Find(const char *k)
    {
        for (auto &m : members)
            if (m.first == k) return &m.second;
        return nullptr;
    }
};

struct Parser {
    const char *p, *e;
    int depth = 0;

    void Ws()
    {
        while (p < e && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
    }
    bool Str(std::string &out)
    {
        if (p >= e || *p != '"') return false;
        const char *s = ++p;
        while (p < e && *p != '"') {
            if (*p == '\\') ++p;
            ++p;
        }
        if (p >= e) return false;
        out.assign(s, p - s);
        ++p;
        return true;
    }
    bool Val(JVal &v)
    {
        Ws();
        if (p >= e || ++depth > 64) return false;
        bool ok = false;
        if (*p == '{') {
            v.kind = JVal::Obj;
            ++p;
            Ws();
            if (p < e && *p == '}') {
                ++p;
                ok = true;
            } else {
                for (;;) {
                    Ws();
                    std::pair<std::string, JVal> m;
                    if (!Str(m.first)) break;
                    Ws();
                    if (p >= e || *p != ':') break;
                    ++p;
                    if (!Val(m.second)) break;
                    v.members.push_back(std::move(m));
                    Ws();
                    if (p < e && *p == ',') { ++p; continue; }
                    if (p < e && *p == '}') { ++p; ok = true; }
                    break;
                }
            }
        } else if (*p == '[') {
            v.kind = JVal::Arr;
            ++p;
            Ws();
            if (p < e && *p == ']') {
                ++p;
                ok = true;
            } else {
                for (;;) {
                    JVal it;
                    if (!Val(it)) break;
                    v.items.push_back(std::move(it));
                    Ws();
                    if (p < e && *p == ',') { ++p; continue; }
                    if (p < e && *p == ']') { ++p; ok = true; }
                    break;
                }
            }
        } else if (*p == '"') {
            v.kind = JVal::Str;
            ok = Str(v.raw);
        } else {
            const char *s = p;
            while (p < e && (isalnum((unsigned char)*p) || *p == '-' || *p == '+' || *p == '.')) ++p;
            v.raw.assign(s, p - s);
            v.kind = (v.raw == "true" || v.raw == "false" || v.raw == "null") ? JVal::Lit : JVal::Num;
            ok = !v.raw.empty();
        }
        --depth;
        return ok;
    }
};

inline bool Parse(const std::string &text, JVal &root)
{
    Parser ps{ text.data(), text.data() + text.size() };
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
        ps.p += 3;
    if (!ps.Val(root)) return false;
    ps.Ws();
    return ps.p == ps.e;
}

// JSON string escapes -> UTF-8
inline std::string Unescape(const std::string &raw)
{
    std::string o;
    for (size_t i = 0; i < raw.size(); ++i) {
        char c = raw[i];
        if (c != '\\' || i + 1 >= raw.size()) { o += c; continue; }
        char n = raw[++i];
        if (n == 'n') o += '\n';
        else if (n == 't') o += '\t';
        else if (n == 'r') o += '\r';
        else if (n == 'b' || n == 'f') o += ' ';
        else if (n == 'u' && i + 4 < raw.size()) {
            unsigned cp = (unsigned)strtoul(raw.substr(i + 1, 4).c_str(), nullptr, 16);
            i += 4;
            // a surrogate pair (\uD83D\uDE00): one code point above the BMP
            if (cp >= 0xD800 && cp < 0xDC00 && i + 6 < raw.size() && raw[i + 1] == '\\' && raw[i + 2] == 'u') {
                const unsigned lo = (unsigned)strtoul(raw.substr(i + 3, 4).c_str(), nullptr, 16);
                if (lo >= 0xDC00 && lo < 0xE000) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    i += 6;
                }
            }
            if (cp < 0x80) o += (char)cp;
            else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 63)); }
            else if (cp < 0x10000) { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 63)); o += (char)(0x80 | (cp & 63)); }
            else {
                o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 63));
                o += (char)(0x80 | ((cp >> 6) & 63)); o += (char)(0x80 | (cp & 63));
            }
        } else o += n;   // \\ \" \/
    }
    return o;
}


} // namespace app
