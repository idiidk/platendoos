// Renders the platendoos chip (mods/badge_chip.h) as a PNG, e.g. the README's logo.
//   clang++ -std=c++20 -fobjc-arc -framework AppKit -o /tmp/render_chip tools/dev/render_chip.mm && /tmp/render_chip docs/platendoos.png 12
#import <AppKit/AppKit.h>
#include "../../mods/badge_chip.h"

int main(int argc, char** argv) {
    if (argc < 2) return fprintf(stderr, "usage: render_chip out.png [scale]\n"), 1;
    CGFloat scale = argc > 2 ? atof(argv[2]) : 12;
    NSSize size = NSMakeSize(90, 16);
    NSBitmapImageRep* rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr pixelsWide:size.width * scale
        pixelsHigh:size.height * scale bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO
        colorSpaceName:NSCalibratedRGBColorSpace bytesPerRow:0 bitsPerPixel:0];
    rep.size = size;  // points: the drawing code works in rekordbox's sizes, the rep holds scale x the pixels
    [NSGraphicsContext saveGraphicsState];
    NSGraphicsContext.currentContext = [NSGraphicsContext graphicsContextWithBitmapImageRep:rep];
    drawChip(NSMakeRect(0, 0, size.width, size.height));
    [NSGraphicsContext restoreGraphicsState];
    return [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:@(argv[1]) atomically:YES] ? 0 : 1;
}
