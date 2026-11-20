// UTF-8 -> UTF-16 code units, for file formats that store text as UTF-16 (ID3 frames, rekordbox's ANLZ files).
#pragma once
#include <stdint.h>
#include <string>

namespace rbm {

inline std::u16string utf16(const std::string& s) {
    std::u16string out;
    for (size_t i = 0; i < s.size();) {
        uint8_t c = (uint8_t)s[i];
        uint32_t cp;
        int n;
        if (c < 0x80) cp = c, n = 1;
        else if ((c & 0xe0) == 0xc0) cp = c & 0x1f, n = 2;
        else if ((c & 0xf0) == 0xe0) cp = c & 0x0f, n = 3;
        else if ((c & 0xf8) == 0xf0) cp = c & 0x07, n = 4;
        else { out += u'�'; i++; continue; }
        if (i + n > s.size()) { out += u'�'; break; }
        for (int k = 1; k < n; k++) cp = cp << 6 | ((uint8_t)s[i + k] & 0x3f);
        i += n;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out += (char16_t)(0xd800 + (cp >> 10));
            out += (char16_t)(0xdc00 + (cp & 0x3ff));
        } else {
            out += (char16_t)cp;
        }
    }
    return out;
}

// The UTF-16 text as bytes, big- or little-endian (no byte order mark).
inline std::string utf16Bytes(const std::string& s, bool bigEndian) {
    std::string out;
    for (char16_t u : utf16(s)) {
        char hi = (char)(u >> 8), lo = (char)(u & 0xff);
        out += bigEndian ? hi : lo;
        out += bigEndian ? lo : hi;
    }
    return out;
}

}  // namespace rbm
