// Paints rekordbox's main component into an image (JUCE's own snapshot) and writes it as a PNG.
#import <AppKit/AppKit.h>
#import <ImageIO/ImageIO.h>
#include "rbmod.h"
#include <vector>
using namespace rbm;
#ifndef OUT
#define OUT "snap.png"
#endif
#define D "/tmp/rbm-uimon/"
struct R { int x, y, w, h; };
struct Img { void* p; };
struct Buf { Img v; };
extern "C" void rbm_experiment() {
    plat::onMain([] {
        void* mc = at<void* (*)()>(symbol("__ZN13MainComponent11getInstanceEv"))();
        if (!mc) return logf("snap: no MainComponent");
        R r = at<R (*)(void*)>(symbol("__ZNK4juce9Component14getLocalBoundsEv"))(mc);
        void* type[2] = {};
        at<void (*)(void*)>(symbol("__ZN4juce17SoftwareImageTypeC1Ev"))(type);
        Img img{};
        // Image returned through x8: call with the buffer as the hidden result pointer
        typedef void (*Fn)(void*, uint64_t, uint64_t, bool, float, void*);
        Fn fn = at<Fn>(symbol("__ZN4juce9Component23createComponentSnapshotENS_9RectangleIiEEbfRKNS_9ImageTypeE"));
        uint64_t a = (uint32_t)r.x | ((uint64_t)(uint32_t)r.y << 32), b = (uint32_t)r.w | ((uint64_t)(uint32_t)r.h << 32);
        Img (*call)(void*, uint64_t, uint64_t, bool, float, void*) = (Img (*)(void*, uint64_t, uint64_t, bool, float, void*))fn;
        (void)call;
        // arm64: a non-trivially-copyable return value goes through x8; emulate with a struct too big for registers
        struct Big { void* p; void* pad[3]; };
        Big big = ((Big (*)(void*, uint64_t, uint64_t, bool, float, void*))fn)(mc, a, b, true, 2.0f, type);
        img.p = big.p;
        char bd[64] = {};
        at<void (*)(void*, const Img*, int)>(symbol("__ZN4juce5Image10BitmapDataC1ERKS0_NS1_13ReadWriteModeE"))(bd, &img, 0);
        uint8_t* data = *(uint8_t**)bd; int stride = *(int*)(bd + 0x14), w = *(int*)(bd + 0x1c), h = *(int*)(bd + 0x20);
        logf("snap: %dx%d stride %d fmt %d", w, h, stride, *(int*)(bd + 0x10));
        if (data && w > 0 && h > 0) {
            int px = *(int*)(bd + 0x18);
            std::vector<uint8_t> rgba((size_t)w * h * 4);
            for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
                const uint8_t* s = data + (size_t)y * stride + (size_t)x * px; uint8_t* d = &rgba[((size_t)y * w + x) * 4];
                d[0] = s[2], d[1] = s[1], d[2] = s[0], d[3] = 255;
            }
            CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
            CGContextRef ctx = CGBitmapContextCreate(rgba.data(), w, h, 8, (size_t)w * 4, cs, kCGBitmapByteOrder32Big | kCGImageAlphaNoneSkipLast);
            CGImageRef im = CGBitmapContextCreateImage(ctx);
            NSURL* url = [NSURL fileURLWithPath:@D OUT];
            CGImageDestinationRef dst = CGImageDestinationCreateWithURL((__bridge CFURLRef)url, CFSTR("public.png"), 1, nullptr);
            CGImageDestinationAddImage(dst, im, nullptr); CGImageDestinationFinalize(dst);
            CFRelease(dst); CGImageRelease(im); CGContextRelease(ctx); CGColorSpaceRelease(cs);
        }
        if (void* rel = *(void**)(bd + 0x28)) (*(void (***)(void*))rel)[1](rel);
        at<void (*)(Img*)>(symbol("__ZN4juce5ImageD1Ev"))(&img);
        logf("snap: wrote " OUT);
    });
}
