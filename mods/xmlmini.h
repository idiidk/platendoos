// xmlmini: just enough XML for rekordbox's own XML files (well-formed, written by juce): elements with ordered
// attributes, their children kept as raw XML text. Reads the elements of one tag from a document, and writes
// elements back. No DTDs, no CDATA, no namespaces.
#pragma once
#include <stdlib.h>
#include <string.h>
#include <string>
#include <utility>
#include <vector>

namespace xmlmini {

inline std::string escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break;
            case '\'': o += "&apos;"; break;
            case '\n': o += "&#10;"; break;
            case '\r': o += "&#13;"; break;
            case '\t': o += "&#9;"; break;
            default: o += c;
        }
    }
    return o;
}

inline void appendUtf8(std::string& o, unsigned long cp) {
    if (cp < 0x80) o += (char)cp;
    else if (cp < 0x800) o += (char)(0xc0 | cp >> 6), o += (char)(0x80 | (cp & 0x3f));
    else if (cp < 0x10000) o += (char)(0xe0 | cp >> 12), o += (char)(0x80 | (cp >> 6 & 0x3f)), o += (char)(0x80 | (cp & 0x3f));
    else o += (char)(0xf0 | cp >> 18), o += (char)(0x80 | (cp >> 12 & 0x3f)), o += (char)(0x80 | (cp >> 6 & 0x3f)), o += (char)(0x80 | (cp & 0x3f));
}

inline std::string unescape(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '&') { o += s[i]; continue; }
        size_t semi = s.find(';', i);
        if (semi == std::string::npos) { o += s[i]; continue; }
        std::string e = s.substr(i + 1, semi - i - 1);
        if (e == "amp") o += '&';
        else if (e == "lt") o += '<';
        else if (e == "gt") o += '>';
        else if (e == "quot") o += '"';
        else if (e == "apos") o += '\'';
        else if (e.size() > 1 && e[0] == '#') appendUtf8(o, e[1] == 'x' ? strtoul(e.c_str() + 2, nullptr, 16) : strtoul(e.c_str() + 1, nullptr, 10));
        else { o += s[i]; continue; }
        i = semi;
    }
    return o;
}

struct Elem {
    std::string name;
    std::vector<std::pair<std::string, std::string>> attrs;  // in order, unescaped
    std::string inner;                                       // the children as XML text ("" for an empty element)
    std::vector<Elem> children;                              // children added with add(), written after `inner`

    explicit Elem(std::string n = "") : name(std::move(n)) {}
    std::string attr(const std::string& n) const {
        for (auto& a : attrs)
            if (a.first == n) return a.second;
        return "";
    }
    Elem& set(const std::string& n, const std::string& v) {
        for (auto& a : attrs)
            if (a.first == n) return a.second = v, *this;
        attrs.emplace_back(n, v);
        return *this;
    }
    void remove(const std::string& n) {
        for (size_t i = 0; i < attrs.size(); i++)
            if (attrs[i].first == n) return (void)attrs.erase(attrs.begin() + i);
    }
    Elem& add(Elem e) { children.push_back(std::move(e)); return children.back(); }
    size_t childCount() const { return children.size() + count(inner, ""); }
    // How many child elements of this tag `inner` holds ("" = any).
    static size_t count(const std::string& xml, const std::string& tag) {
        size_t n = 0;
        int depth = 0;
        for (size_t i = xml.find('<'); i != std::string::npos; i = xml.find('<', i + 1)) {
            if (xml.compare(i, 2, "</") == 0) { depth--; continue; }
            if (xml.compare(i, 2, "<?") == 0 || xml.compare(i, 2, "<!") == 0) continue;
            size_t end = xml.find('>', i);
            if (end == std::string::npos) break;
            bool selfClosing = xml[end - 1] == '/';
            if (depth == 0) {
                size_t nameEnd = xml.find_first_of(" \t\r\n/>", i + 1);
                if (tag.empty() || xml.compare(i + 1, nameEnd - i - 1, tag) == 0) n++;
            }
            if (!selfClosing) depth++;
        }
        return n;
    }
    std::string xml(int indent = 0) const {
        std::string pad(indent, ' ');
        std::string o = pad + "<" + name;
        for (auto& a : attrs) o += " " + a.first + "=\"" + escape(a.second) + "\"";
        if (inner.empty() && children.empty()) return o + "/>\n";
        o += ">";
        if (!inner.empty()) o += inner;
        if (!children.empty()) {
            o += "\n";
            for (const Elem& c : children) o += c.xml(indent + 2);
            o += pad;
        }
        return o + "</" + name + ">\n";
    }
};

// The elements <tag ...> of the document, wherever they are (not inside each other).
inline std::vector<Elem> elements(const std::string& doc, const std::string& tag) {
    std::vector<Elem> out;
    std::string open = "<" + tag;
    for (size_t i = doc.find(open); i != std::string::npos; i = doc.find(open, i + 1)) {
        char after = i + open.size() < doc.size() ? doc[i + open.size()] : 0;
        if (after != ' ' && after != '>' && after != '/' && after != '\n' && after != '\t' && after != '\r') continue;
        Elem e(tag);
        size_t p = i + open.size();
        for (;;) {  // attributes
            while (p < doc.size() && strchr(" \t\r\n", doc[p])) p++;
            if (p >= doc.size() || doc[p] == '>' || doc[p] == '/') break;
            size_t eq = doc.find('=', p);
            if (eq == std::string::npos) return out;
            std::string name = doc.substr(p, eq - p);
            while (!name.empty() && strchr(" \t\r\n", name.back())) name.pop_back();
            size_t q = doc.find_first_of("\"'", eq);
            if (q == std::string::npos) return out;
            size_t qe = doc.find(doc[q], q + 1);
            if (qe == std::string::npos) return out;
            e.attrs.emplace_back(name, unescape(doc.substr(q + 1, qe - q - 1)));
            p = qe + 1;
        }
        if (p >= doc.size()) break;
        if (doc[p] == '/') {  // <tag .../>
            out.push_back(std::move(e));
            i = p;
            continue;
        }
        size_t close = doc.find("</" + tag + ">", p);  // (rekordbox doesn't nest an element in one of its own tag)
        if (close == std::string::npos) break;
        e.inner = doc.substr(p + 1, close - p - 1);
        if (e.inner.find_first_not_of(" \t\r\n") == std::string::npos) e.inner.clear();
        out.push_back(std::move(e));
        i = close;
    }
    return out;
}

inline std::string document(const Elem& root) { return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n\n" + root.xml(); }

}  // namespace xmlmini
