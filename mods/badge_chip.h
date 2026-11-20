// The platendoos chip's drawing, shared by the badge in rekordbox (mods/badge.mm) and the README's logo
// (tools/dev/render_chip.mm). Draws into the current graphics context, filling b (90 x 16 pt in rekordbox).
#pragma once
#import <AppKit/AppKit.h>

// The chip's icon on its own: the blue tile with the record, filling tile (16 x 16 pt in the chip). Also the
// windows' and the patcher's icon (tools/dev/render_icon.mm). hint: on whole pixels (at 1 pixel per point, for
// the small icon sizes: the thin groove otherwise smears).
// fill: the tile's colour (default the chip's blue; the badge pulses it orange while a restart is pending).
static void drawChipIcon(NSRect tile, bool hint = false, NSColor* fill = nil) {
    CGFloat s = tile.size.height / 16;  // the chip's sizes are for a 16 pt tile
    [(fill ?: [NSColor colorWithSRGBRed:0.16 green:0.47 blue:0.92 alpha:1]) setFill];
    [[NSBezierPath bezierPathWithRoundedRect:tile xRadius:3 * s yRadius:3 * s] fill];
    NSPoint c = NSMakePoint(NSMidX(tile), NSMidY(tile));
    CGFloat r = tile.size.height * 0.31, w = 1.2 * s, groove = r * 0.55, gw = 0.6 * s, dot = s;
    if (hint) {  // each ring's edges on pixel boundaries
        w = fmax(1, round(w)), gw = fmax(1, round(gw)), dot = fmax(1, round(dot));
        r = round(r + w / 2) - w / 2;
        groove = round(groove + gw / 2) - gw / 2;
        if (groove + gw / 2 > r - w / 2 - 1) groove = r - w / 2 - 1 - gw / 2;  // a gap between the rings
    }
    [[NSColor whiteColor] set];
    NSBezierPath* disc = [NSBezierPath bezierPathWithOvalInRect:NSMakeRect(c.x - r, c.y - r, 2 * r, 2 * r)];
    disc.lineWidth = w;
    [disc stroke];
    if (!hint || tile.size.height > 16) {  // at 16 pixels the groove leaves no gaps: ring and hole only
        NSBezierPath* inner = [NSBezierPath bezierPathWithOvalInRect:NSMakeRect(c.x - groove, c.y - groove, 2 * groove, 2 * groove)];
        inner.lineWidth = gw;
        [inner stroke];
    }
    [[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(c.x - dot, c.y - dot, 2 * dot, 2 * dot)] fill];
}

static void drawChip(NSRect b) {
    // In rekordbox's own button style: a dark grey tile with small corners, its blue for the icon (like the FX
    // button) and bold capitals (like PERFORMANCE). The icon is a record, platendoos being a box of them.
    [[NSColor colorWithCalibratedWhite:0.19 alpha:1] setFill];
    [[NSBezierPath bezierPathWithRoundedRect:b xRadius:3 yRadius:3] fill];
    NSRect tile = NSMakeRect(0, 0, b.size.height, b.size.height);
    drawChipIcon(tile);

    NSFont* font = [NSFont systemFontOfSize:9 weight:NSFontWeightBold];
    NSMutableAttributedString* text = [[NSMutableAttributedString alloc] initWithString:@"PLATENDOOS"
        attributes:@{NSFontAttributeName : font, NSForegroundColorAttributeName : [NSColor colorWithCalibratedWhite:0.88 alpha:1]}];
    [text addAttribute:NSKernAttributeName value:@0.6 range:NSMakeRange(0, text.length - 1)];
    // Centred in the space right of the tile; without NSStringDrawingUsesLineFragmentOrigin the rect's origin
    // is the baseline, so the capitals' height is what gets centred.
    CGFloat x = NSMaxX(tile) + round((b.size.width - NSMaxX(tile) - text.size.width) / 2);
    [text drawWithRect:NSMakeRect(x, (b.size.height - font.capHeight) / 2, text.size.width, 0) options:0];
}
