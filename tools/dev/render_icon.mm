// Renders the platendoos icon (the chip's record tile, mods/badge_chip.h's drawChipIcon) as a PNG.
//   render_icon out.png <pixels> [mac]
// mac: on macOS's app icon grid (the tile 824/1024 of the canvas, its corners ~22%), for the patcher's .icns;
// otherwise the tile fills the image (Windows' window and exe icons).
#import <AppKit/AppKit.h>
#include "../../mods/badge_chip.h"

int main(int argc, char** argv) {
    if (argc < 3) return fprintf(stderr, "usage: render_icon out.png pixels [mac]\n"), 1;
    int px = atoi(argv[2]);
    bool mac = argc > 3 && !strcmp(argv[3], "mac");
    NSBitmapImageRep* rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr pixelsWide:px pixelsHigh:px
        bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSCalibratedRGBColorSpace bytesPerRow:0 bitsPerPixel:0];
    rep.size = NSMakeSize(px, px);
    [NSGraphicsContext saveGraphicsState];
    NSGraphicsContext.currentContext = [NSGraphicsContext graphicsContextWithBitmapImageRep:rep];
    NSGraphicsContext.currentContext.shouldAntialias = YES;
    if (mac) {
        // macOS rounds its icons more than the chip's 3/16: the tile at the grid's size and corners, the record as in the chip
        CGFloat t = px * 824.0 / 1024, o = (px - t) / 2;
        NSRect tile = NSMakeRect(o, o, t, t);
        [[NSColor colorWithSRGBRed:0.16 green:0.47 blue:0.92 alpha:1] setFill];
        [[NSBezierPath bezierPathWithRoundedRect:tile xRadius:t * 0.225 yRadius:t * 0.225] fill];
        // the chip's icon, a little larger than the tile and clipped to it: macOS's corners instead of the chip's
        NSBezierPath* clip = [NSBezierPath bezierPathWithRoundedRect:tile xRadius:t * 0.225 yRadius:t * 0.225];
        [clip addClip];
        drawChipIcon(NSInsetRect(tile, -t * 0.02, -t * 0.02));
    } else {
        drawChipIcon(NSMakeRect(0, 0, px, px), px < 40);
    }
    [NSGraphicsContext restoreGraphicsState];
    return [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:@(argv[1]) atomically:YES] ? 0 : 1;
}
