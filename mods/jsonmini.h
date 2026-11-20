// A small JSON reader (header-only) for answers from web APIs: parses into a tree; never throws.
#pragma once
#include <map>
#include <string>
#include <vector>
#include <stdlib.h>
#include <string.h>

namespace jsonmini {

struct Value {
    enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Value> a;
    std::map<std::string, Value> o;
    const Value& operator[](const char* k) const {
        static const Value none;
        auto it = o.find(k);
        return it == o.end() ? none : it->second;
    }
    const Value& operator[](size_t i) const {
        static const Value none;
        return i < a.size() ? a[i] : none;
    }
};

namespace detail {
inline void ws(const std::string& t, size_t& i) { while (i < t.size() && strchr(" \t\r\n", t[i])) i++; }
inline void utf8(std::string& s, unsigned c) {
    if (c < 0x80) s += (char)c;
    else if (c < 0x800) s += (char)(0xC0 | c >> 6), s += (char)(0x80 | (c & 63));
    else if (c < 0x10000) s += (char)(0xE0 | c >> 12), s += (char)(0x80 | (c >> 6 & 63)), s += (char)(0x80 | (c & 63));
    else s += (char)(0xF0 | c >> 18), s += (char)(0x80 | (c >> 12 & 63)), s += (char)(0x80 | (c >> 6 & 63)), s += (char)(0x80 | (c & 63));
}
inline bool str(const std::string& t, size_t& i, std::string* out) {
    if (i >= t.size() || t[i] != '"') return false;
    for (i++; i < t.size(); i++) {
        char c = t[i];
        if (c == '"') return i++, true;
        if (c != '\\') { *out += c; continue; }
        if (++i >= t.size()) return false;
        switch (t[i]) {
            case 'n': *out += '\n'; break;
            case 't': *out += '\t'; break;
            case 'r': *out += '\r'; break;
            case 'b': *out += '\b'; break;
            case 'f': *out += '\f'; break;
            case 'u': {
                if (i + 4 >= t.size()) return false;
                unsigned c1 = (unsigned)strtoul(t.substr(i + 1, 4).c_str(), nullptr, 16);
                i += 4;
                if (c1 >= 0xD800 && c1 < 0xDC00 && i + 6 < t.size() && t[i + 1] == '\\' && t[i + 2] == 'u') {
                    unsigned c2 = (unsigned)strtoul(t.substr(i + 3, 4).c_str(), nullptr, 16);
                    if (c2 >= 0xDC00 && c2 < 0xE000) c1 = 0x10000 + ((c1 - 0xD800) << 10) + (c2 - 0xDC00), i += 6;
                }
                utf8(*out, c1);
                break;
            }
            default: *out += t[i];
        }
    }
    return false;
}
inline bool val(const std::string& t, size_t& i, Value* v, int depth) {
    if (depth > 64) return false;
    ws(t, i);
    if (i >= t.size()) return false;
    char c = t[i];
    if (c == '{') {
        v->type = Value::Obj;
        i++, ws(t, i);
        if (i < t.size() && t[i] == '}') return i++, true;
        for (;;) {
            std::string k;
            ws(t, i);
            if (!str(t, i, &k)) return false;
            ws(t, i);
            if (i >= t.size() || t[i++] != ':') return false;
            if (!val(t, i, &v->o[k], depth + 1)) return false;
            ws(t, i);
            if (i >= t.size()) return false;
            if (t[i] == ',') { i++; continue; }
            return t[i++] == '}';
        }
    }
    if (c == '[') {
        v->type = Value::Arr;
        i++, ws(t, i);
        if (i < t.size() && t[i] == ']') return i++, true;
        for (;;) {
            v->a.emplace_back();
            if (!val(t, i, &v->a.back(), depth + 1)) return false;
            ws(t, i);
            if (i >= t.size()) return false;
            if (t[i] == ',') { i++; continue; }
            return t[i++] == ']';
        }
    }
    if (c == '"') return v->type = Value::Str, str(t, i, &v->s);
    if (!t.compare(i, 4, "true")) return v->type = Value::Bool, v->b = true, i += 4, true;
    if (!t.compare(i, 5, "false")) return v->type = Value::Bool, i += 5, true;
    if (!t.compare(i, 4, "null")) return i += 4, true;
    char* end = nullptr;
    v->n = strtod(t.c_str() + i, &end);
    if (end == t.c_str() + i) return false;
    v->type = Value::Num;
    i = end - t.c_str();
    return true;
}
}  // namespace detail

inline bool parse(const std::string& text, Value* out) {
    size_t i = 0;
    return detail::val(text, i, out, 0);
}

}  // namespace jsonmini
