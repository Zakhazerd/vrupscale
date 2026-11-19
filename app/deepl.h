// Translation by DeepL's API (https://api-free.deepl.com or https://api.deepl.com, /v2/translate),
// with the user's own key. Only the texts go out, with the key in a header; nothing is sent unless a
// key is set. Free keys end in ":fx" and belong to api-free.deepl.com.
//
// VRUPSCALE_DEEPL_URL (tests) replaces the server, and only by one on this computer: http://127.0.0.1
// or http://localhost with a port.
#pragma once

#include <stdint.h>
#include <string>
#include <vector>

namespace app {

struct DeepLResult {
    bool ok = false;
    int status = 0;                   // HTTP status (0: no answer)
    std::vector<std::wstring> texts;  // one per text asked
    std::wstring error;               // for the page
    bool fatal = false;               // asking again won't help until something changes: the key was refused (403),
                                      // or this month's characters are used up (456)
};
// source, target: language tags as the app keeps them ("ja", "en-US").
DeepLResult DeepLTranslate(const std::wstring &key, const std::vector<std::wstring> &texts, const std::wstring &source,
                           const std::wstring &target);

struct DeepLUsage {
    bool ok = false;
    uint64_t count = 0, limit = 0;    // characters translated in the billing period, and its allowance
    std::wstring error;
};
DeepLUsage DeepLGetUsage(const std::wstring &key);

// Could this be a DeepL key (its shape only)? Keys are 36 characters like a GUID, free ones with ":fx".
bool DeepLKeyPlausible(const std::wstring &key);
std::wstring DeepLServer(const std::wstring &key);   // where requests with this key go (for the page)

int DeepLSelfTest(std::wstring &report);   // the request and answer handling (no network)

} // namespace app
