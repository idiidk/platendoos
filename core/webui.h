// rbm::webui: the mods' tool windows are HTML pages (ui/*.html, shipped in resourcesDir()/ui) shown in a native
// window with a web view: WKWebView on macOS (core/webui_mac.mm), WebView2 on Windows (core/webui_win.cpp).
// Page side (ui/common.js): window.rbm.send(obj) sends obj to onMessage as JSON; window.rbm.onState = fn(obj)
// receives what post() sends. All functions are for the main thread.
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <functional>
#include <string>

namespace rbm::webui {

struct Window;

// Opens the tool window `id` (or brings it to the front if it is open or was closed before) showing
// ui/<page>.html. onMessage gets the JSON text the page sent.
Window* open(const char* id, const char* page, const char* title, int width, int height,
             std::function<void(const std::string& json)> onMessage);
// The page gets json (a JSON value) in window.rbm.onState(value); queued until the page has loaded.
void post(Window* w, const std::string& json);
bool visible(Window* w);
// A tool window stays above rekordbox's own windows while rekordbox is the active app (macOS: the floating level;
// Windows: owned by rekordbox's main window). floats=false makes it an ordinary window that rekordbox's main window
// can cover, as a big window someone works in for a while should be (the Preferences window). Any time after open().
void setFloating(Window* w, bool floats);
// Open and actually seen: not minimised or fully covered/on another desktop. For periodic refreshes, which
// can skip the work meanwhile (the next tick after it is seen again sends the current state).
bool onScreen(Window* w);
void close(Window* w);    // hides it (open() shows it again)

// ---- tiny JSON helpers for the mods' state and commands ------------------------------------------------

// How many bytes the UTF-8 character at s[i] takes (1-4), or 0 if what is there is not valid UTF-8 (a stray or
// missing continuation byte, an overlong form, a surrogate, past U+10FFFF).
inline size_t utf8Length(const std::string& s, size_t i) {
    unsigned char c = (unsigned char)s[i];
    if (c < 0x80) return 1;
    size_t n = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC2 ? 2 : 0;
    if (!n || c > 0xF4 || i + n > s.size()) return 0;
    for (size_t k = 1; k < n; k++)
        if (((unsigned char)s[i + k] & 0xC0) != 0x80) return 0;
    unsigned char d = (unsigned char)s[i + 1];
    if ((c == 0xE0 && d < 0xA0) || (c == 0xED && d > 0x9F) || (c == 0xF0 && d < 0x90) || (c == 0xF4 && d > 0x8F)) return 0;
    return n;
}

// The first `max` bytes of UTF-8 text at most, never cut inside a character.
inline std::string utf8Prefix(const std::string& s, size_t max) {
    if (s.size() <= max) return s;
    while (max > 0 && ((unsigned char)s[max] & 0xC0) == 0x80) max--;
    return s.substr(0, max);
}

// A JSON string literal (with quotes) for UTF-8 text. Bytes that are not UTF-8 (a tag in another encoding, text cut
// inside a character) become U+FFFD: one such byte would otherwise make the whole message unreadable for the page.
inline std::string jsonStr(const std::string& s) {
    std::string o = "\"";
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        size_t n = utf8Length(s, i);
        if (n != 1) {
            if (n) o.append(s, i, n), i += n;
            else o += "\xEF\xBF\xBD", i++;
            continue;
        }
        i++;
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
                else o += (char)c;
        }
    }
    // U+2028/2029 (E2 80 A8/A9) end a line in JavaScript source; escape them for evaluateJavaScript
    for (size_t p; (p = o.find("\xE2\x80\xA8")) != std::string::npos;) o.replace(p, 3, "\\u2028");
    for (size_t p; (p = o.find("\xE2\x80\xA9")) != std::string::npos;) o.replace(p, 3, "\\u2029");
    return o + "\"";
}

// Writing JSON for a page: an object or an array whose members are added in order, with the escaping done here.
//     Json o; o.add("title", "Stems").add("on", true).add("n", 3).raw("settings", other.text());
//     Json a = Json::array(); a.item(o).item(std::string("x"));
// A number that isn't finite is written as null. text() is the JSON so far, closed.
class Json {
public:
    Json() : s_("{"), close_('}') {}
    static Json array() { Json j; j.s_ = "["; j.close_ = ']'; return j; }
    Json& add(const char* key, const std::string& v) { return raw(key, jsonStr(v)); }
    Json& add(const char* key, const char* v) { return raw(key, jsonStr(v ? v : "")); }
    Json& add(const char* key, bool v) { return raw(key, v ? "true" : "false"); }
    Json& add(const char* key, int v) { return raw(key, std::to_string(v)); }
    Json& add(const char* key, double v) { return raw(key, number(v)); }
    Json& add(const char* key, const Json& v) { return raw(key, v.text()); }
    Json& raw(const char* key, const std::string& json) {  // a value that is JSON already
        comma();
        s_ += jsonStr(key) + ":" + json;
        return *this;
    }
    // (arrays)
    Json& item(const Json& v) { return itemRaw(v.text()); }
    Json& item(const std::string& v) { return itemRaw(jsonStr(v)); }
    Json& item(const char* v) { return itemRaw(jsonStr(v ? v : "")); }
    Json& itemRaw(const std::string& json) {
        comma();
        s_ += json;
        return *this;
    }
    bool empty() const { return s_.size() == 1; }
    std::string text() const { return s_ + close_; }
    static std::string number(double v) {
        if (!(v == v) || v > 1e300 || v < -1e300) return "null";
        char b[32];
        snprintf(b, sizeof b, "%.10g", v);
        return b;
    }

private:
    void comma() { if (s_.size() > 1) s_ += ','; }
    std::string s_;
    char close_;
};

// The value of a top-level-ish key in a flat JSON object the page sent: strings unescaped, numbers and
// true/false as their text. "" if absent.
inline std::string jsonGet(const std::string& json, const char* key) {
    std::string k = std::string("\"") + key + "\"";
    size_t p = 0;
    for (;;) {
        p = json.find(k, p);
        if (p == std::string::npos) return "";
        size_t q = p + k.size();
        while (q < json.size() && json[q] == ' ') q++;
        if (q < json.size() && json[q] == ':') { p = q + 1; break; }
        p += k.size();
    }
    while (p < json.size() && json[p] == ' ') p++;
    std::string out;
    if (p < json.size() && json[p] == '"') {
        for (p++; p < json.size() && json[p] != '"'; p++) {
            if (json[p] != '\\' || p + 1 >= json.size()) { out += json[p]; continue; }
            char c = json[++p];
            switch (c) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u': {
                    unsigned cp = (unsigned)strtoul(json.substr(p + 1, 4).c_str(), nullptr, 16);
                    p += 4;
                    if (cp >= 0xD800 && cp < 0xDC00 && p + 3 < json.size() && json.compare(p + 1, 2, "\\u") == 0) {
                        unsigned lo = (unsigned)strtoul(json.substr(p + 3, 4).c_str(), nullptr, 16);
                        if (lo >= 0xDC00 && lo < 0xE000) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            p += 6;
                        }
                    }
                    if (cp >= 0xD800 && cp < 0xE000) cp = 0xFFFD;  // half a pair
                    if (cp < 0x80) out += (char)cp;
                    else if (cp < 0x800) { out += (char)(0xC0 | cp >> 6); out += (char)(0x80 | (cp & 63)); }
                    else if (cp < 0x10000) { out += (char)(0xE0 | cp >> 12); out += (char)(0x80 | (cp >> 6 & 63)); out += (char)(0x80 | (cp & 63)); }
                    else { out += (char)(0xF0 | cp >> 18); out += (char)(0x80 | (cp >> 12 & 63)); out += (char)(0x80 | (cp >> 6 & 63)); out += (char)(0x80 | (cp & 63)); }
                    break;
                }
                default: out += c;
            }
        }
    } else {
        while (p < json.size() && json[p] != ',' && json[p] != '}' && json[p] != ' ') out += json[p++];
    }
    return out;
}

}  // namespace rbm::webui
