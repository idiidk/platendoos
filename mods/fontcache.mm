// mac-only: caches CoreText CTFonts for HarfBuzz; no Windows equivalent, leave it out of a Windows build.
// fontcache: makes text drawing (most of the track list's paint time) cheaper.
//
// JUCE's text shaping builds a fresh HarfBuzz font for every run of text it draws, and HarfBuzz's
// CoreText backend then creates a CTFont for it (create_ct_font: CTFontCreateWithGraphicsFont plus
// font-descriptor work). Profiling showed that at ~17% of the main thread while lists repaint.
// CTFonts are immutable, so this hands out a shared one per (CGFont, size) instead.
//   config: fontcache=0 to disable
#include "../core/rbmod.h"
#include <CoreText/CoreText.h>
#include <map>
#include <mutex>
#include <utility>

using namespace rbm;

static volatile bool g_on = true;
static std::mutex g_lock;
static std::map<std::pair<CGFontRef, double>, CTFontRef> g_fonts;  // keys hold a retain on the CGFont

// Returns a +1 reference, like the original (the caller releases it when its font data dies).
extern "C" CTFontRef rbm_hook_font_createCT(CGFontRef cgFont, double size) {
    auto orig = RBM_ORIG(font_createCT, CTFontRef (*)(CGFontRef, double));
    if (!g_on || !cgFont) return orig(cgFont, size);
    std::lock_guard<std::mutex> l(g_lock);
    auto it = g_fonts.find({cgFont, size});
    if (it != g_fonts.end()) return (CTFontRef)CFRetain(it->second);
    CTFontRef f = orig(cgFont, size);
    if (f && g_fonts.size() < 512) {
        CFRetain(cgFont);  // so the pointer can't be reused by another font while it's a key
        g_fonts[{cgFont, size}] = (CTFontRef)CFRetain(f);
    }
    return f;
}

RBM_MOD({.name = "fontcache", .enabled = &g_on,
         .title = "Text drawing",
         .about = "Makes drawing text a lot cheaper, so scrolling the track list feels lighter.",
         .area = "speed", .order = 40});
