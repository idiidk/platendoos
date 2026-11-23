// platendoos patcher on macOS: ~/Applications/platendoos.app from the installed rekordbox (what build.sh does,
// without Python, Xcode or the repository): a copy of rekordbox.app whose binary loads librbmods.dylib, with the
// mods' files, the FLX4 mapping and an inverted icon, signed ad hoc for this Mac.
#import <AppKit/AppKit.h>
#include "patcher.h"
#include "macho.h"
#include "flx4map.h"
#include "../core/plat.h"
#include "../core/rbmod.h"
#include <copyfile.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/sysctl.h>

namespace {

const char* kTestedUuid = "D827BD186918357A9F57C506CF428700";  // rekordbox 7.2.19 arm64 (core/image_mac.mm)
const char* kCheckedUuid = "4AF5D59D5EC937DCBD893FBC84D970AD";  // rekordbox 7.2.20 arm64: every mod was run on it

// entitlements.plist (keep in sync)
const char* kEntitlements =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?><!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
    "\"https://www.apple.com/DTDs/PropertyList-1.0.dtd\"><plist version=\"1.0\"><dict>"
    "<key>com.apple.security.cs.allow-unsigned-executable-memory</key><true/>"
    "<key>com.apple.security.cs.disable-library-validation</key><true/>"
    "<key>com.apple.security.device.audio-input</key><true/>"
    "<key>com.apple.security.device.camera</key><true/></dict></plist>";

static double g_quitAt;  // when an installed platendoos that was running was last quit (CFAbsoluteTime; 0: never)

// tools/build/quit.sh (keep in sync): quits a rekordbox app by path and the rekordboxAgent it started.
const char* kQuitScript = R"(app=$1
pgrep -f "^$app/Contents/MacOS/rekordbox$" >/dev/null && osascript -e "with timeout of 15 seconds" -e "tell application \"$app\" to quit" -e "end timeout" 2>/dev/null
for i in {1..40}; do pgrep -f "^$app/Contents/MacOS/rekordbox$" >/dev/null || break; sleep 1; done
pgrep -f "^$app/Contents/MacOS/rekordbox$" >/dev/null && { pkill -f "^$app/Contents/MacOS/rekordbox$"; sleep 2; }
for i in {1..10}; do pgrep -f "^$app/Contents/MacOS/rekordboxAgent" >/dev/null || exit 0; sleep 1; done
pkill -f "^$app/Contents/MacOS/rekordboxAgent"
sleep 1
exit 0)";

NSString* ns(const std::string& s) { return [NSString stringWithUTF8String:s.c_str()] ?: @""; }

// An Intel Mac (it runs rekordbox's x86_64 code); an Apple Silicon Mac says arm64 even to a translated process.
bool intelMac() {
    int arm = 0;
    size_t size = sizeof arm;
    return sysctlbyname("hw.optional.arm64", &arm, &size, nullptr, 0) != 0 || !arm;
}

// "x of y found" for the x86_64 slice, with the first hooks left out ("" when all are there or it has none).
std::string intelSites(const macho::Result& r) {
    const macho::ArchResult* x = r.arch(macho::kX86_64);
    if (!x || x->patched == x->total) return "";
    std::string s = std::to_string(x->patched) + " of " + std::to_string(x->total) + " found";
    for (size_t i = 0; i < x->skipped.size() && i < 3; i++) s += (i ? "; " : ": ") + x->skipped[i];
    if (x->skipped.size() > 3) s += "; …";
    return s;
}

std::string str(NSString* s) { return s ? std::string(s.UTF8String) : std::string(); }

std::string destPath() {
    if (const char* d = getenv("PLATENDOOS_DEST")) return d;
    return str([NSHomeDirectory() stringByAppendingPathComponent:@"Applications/platendoos.app"]); }

// Runs a tool; its output (both streams) in *out. The exit status.
int run(NSString* tool, NSArray<NSString*>* args, std::string* out = nullptr) {
    NSTask* t = [NSTask new];
    t.executableURL = [NSURL fileURLWithPath:tool];
    t.arguments = args;
    NSPipe* p = [NSPipe pipe];
    t.standardOutput = p;
    t.standardError = p;
    NSError* err = nil;
    if (![t launchAndReturnError:&err]) {
        if (out) *out = str(err.localizedDescription);
        return -1;
    }
    NSData* d = [p.fileHandleForReading readDataToEndOfFile];
    [t waitUntilExit];
    if (out) *out = std::string((const char*)d.bytes, d.length);
    return t.terminationStatus;
}

bool readAll(const std::string& path, std::vector<uint8_t>* out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out->resize(n > 0 ? n : 0);
    bool ok = n > 0 && fread(out->data(), 1, n, f) == (size_t)n;
    fclose(f);
    return ok;
}

bool writeAll(const std::string& path, const void* p, size_t n) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(p, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}

uint64_t treeSize(NSString* dir) {
    uint64_t n = 0;
    NSDirectoryEnumerator* e = [NSFileManager.defaultManager enumeratorAtURL:[NSURL fileURLWithPath:dir]
                                                  includingPropertiesForKeys:@[ NSURLFileSizeKey ]
                                                                     options:0
                                                                errorHandler:nil];
    for (NSURL* u in e) {
        NSNumber* s = nil;
        [u getResourceValue:&s forKey:NSURLFileSizeKey error:nil];
        n += s.unsignedLongLongValue;
    }
    return n;
}

// The rekordbox apps in /Applications, newest version first.
std::vector<std::string> candidates() {
    std::vector<std::pair<std::string, std::string>> found;  // version, app
    NSFileManager* fm = NSFileManager.defaultManager;
    NSMutableArray* paths = [NSMutableArray arrayWithObject:@"/Applications/rekordbox.app"];
    // PLATENDOOS_SOURCE: a rekordbox.app elsewhere, instead of the installed ones (testing another version, e.g. one
    // unpacked from its installer with pkgutil --expand-full)
    if (const char* s = getenv("PLATENDOOS_SOURCE")) paths = [NSMutableArray arrayWithObject:ns(s)];
    else for (NSString* d in [fm contentsOfDirectoryAtPath:@"/Applications" error:nil])
        if ([d.lowercaseString hasPrefix:@"rekordbox"] && ![d hasSuffix:@".app"])
            [paths addObject:[NSString stringWithFormat:@"/Applications/%@/rekordbox.app", d]];
    for (NSString* p in paths) {
        NSDictionary* info = [NSDictionary dictionaryWithContentsOfFile:[p stringByAppendingPathComponent:@"Contents/Info.plist"]];
        if (!info) continue;
        found.push_back({str(info[@"CFBundleShortVersionString"]), str(p)});
    }
    std::sort(found.begin(), found.end(), [](auto& a, auto& b) {
        return [ns(a.first) compare:ns(b.first) options:NSNumericSearch] == NSOrderedDescending;
    });
    std::vector<std::string> out;
    for (auto& f : found) out.push_back(f.second);
    return out;
}

bool runningAt(const std::string& app) {
    NSURL* u = [NSURL fileURLWithPath:ns(app)].URLByStandardizingPath;
    for (NSRunningApplication* a in NSWorkspace.sharedWorkspace.runningApplications)
        if ([a.bundleURL.URLByStandardizingPath isEqual:u]) return true;
    return false;
}

// tools/build/invert_icon.swift (keep in sync): rekordbox's icon inverted, so the two apps are easy to tell apart.
bool invertIcon(NSString* icns, NSString* outIcns, std::string* why) {
    NSString* tmp = [NSTemporaryDirectory() stringByAppendingPathComponent:[NSString stringWithFormat:@"platendoos-icon-%d", getpid()]];
    NSString* in = [tmp stringByAppendingPathComponent:@"in.iconset"], *out = [tmp stringByAppendingPathComponent:@"out.iconset"];
    NSFileManager* fm = NSFileManager.defaultManager;
    [fm removeItemAtPath:tmp error:nil];
    [fm createDirectoryAtPath:out withIntermediateDirectories:YES attributes:nil error:nil];
    if (run(@"/usr/bin/iconutil", @[ @"-c", @"iconset", @"-o", in, icns ], why) != 0) return false;
    for (NSString* name in [fm contentsOfDirectoryAtPath:in error:nil]) {
        if (![name hasSuffix:@".png"]) continue;
        NSBitmapImageRep* src = [NSBitmapImageRep imageRepWithData:[NSData dataWithContentsOfFile:[in stringByAppendingPathComponent:name]]];
        if (!src) continue;
        NSInteger w = src.pixelsWide, h = src.pixelsHigh;
        auto rep = [&] {
            return [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr pixelsWide:w pixelsHigh:h bitsPerSample:8 samplesPerPixel:4
                                                             hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace
                                                          bytesPerRow:0 bitsPerPixel:0];
        };
        NSBitmapImageRep* inv = rep();
        for (NSInteger y = 0; y < h; y++)
            for (NSInteger x = 0; x < w; x++) {
                NSColor* c = [[src colorAtX:x y:y] colorUsingColorSpace:NSColorSpace.deviceRGBColorSpace];
                CGFloat a = c.alphaComponent, bg = 1 - 0.13;
                auto f = [&](CGFloat v) { return (1 - v) * a + bg * (1 - a); };
                [inv setColor:[NSColor colorWithDeviceRed:f(c.redComponent) green:f(c.greenComponent) blue:f(c.blueComponent) alpha:1] atX:x y:y];
            }
        const CGFloat scale = 0.80;
        NSBitmapImageRep* fin = rep();
        [NSGraphicsContext saveGraphicsState];
        NSGraphicsContext* ctx = [NSGraphicsContext graphicsContextWithBitmapImageRep:fin];
        ctx.imageInterpolation = NSImageInterpolationHigh;
        NSGraphicsContext.currentContext = ctx;
        [[inv colorAtX:w / 2 y:h * 3 / 100 + 1] setFill];
        NSRectFill(NSMakeRect(0, 0, w, h));
        CGFloat W = w, H = h;
        NSRect part = NSMakeRect(W * 0.055, H * 0.055, W * 0.89, H * 0.89);
        NSRect dst = NSMakeRect(W * (0.5 - 0.445 * scale), H * (0.5 - 0.445 * scale), part.size.width * scale, part.size.height * scale);
        [inv drawInRect:dst fromRect:part operation:NSCompositingOperationCopy fraction:1 respectFlipped:NO hints:nil];
        [NSGraphicsContext restoreGraphicsState];
        [[fin representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:[out stringByAppendingPathComponent:name] atomically:YES];
    }
    bool ok = run(@"/usr/bin/iconutil", @[ @"-c", @"icns", @"-o", outIcns, out ], why) == 0;
    [fm removeItemAtPath:tmp error:nil];
    return ok;
}

std::string payload() { return str([NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"payload"]); }

}  // namespace

namespace rbm {
void logf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}
namespace plat {
std::string resourcesDir() { return payload() + "/rbmods"; }
}  // namespace plat
}  // namespace rbm

namespace patcher {

std::string platformName() { return "macOS"; }
std::string configFile() { return str(NSHomeDirectory()) + "/Library/Application Support/rekordbox-mods/config.txt"; }
std::string payloadDir() { return payload(); }
std::string patcherVersion() { return str([NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleShortVersionString"]); }

void onMain(std::function<void()> fn) {
    dispatch_async(dispatch_get_main_queue(), ^{ fn(); });
}
void background(std::function<void()> fn) {
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ fn(); });
}
void quit() { [NSApp terminate:nil]; }

Target find() {
    Target t;
    t.dest = destPath();
    t.installed = [NSFileManager.defaultManager fileExistsAtPath:ns(t.dest)];
    auto add = [&](const char* state, std::string label, std::string detail) { t.checks.push_back({state, label, detail}); };
    auto say = [&](const char* state, std::string text) { t.notes.push_back({state, "", text}); };

    std::string pay = payload();
    NSFileManager* fm = NSFileManager.defaultManager;
    bool payOk = [fm fileExistsAtPath:ns(pay + "/librbmods.dylib")] && [fm fileExistsAtPath:ns(pay + "/rbmods/ui")];
    uint64_t paySize = treeSize(ns(pay));

    std::vector<std::string> apps = candidates();
    if (apps.empty()) {
        t.problem = "rekordbox isn't installed: there's no rekordbox in /Applications.";
        add("fail", "rekordbox", "not found in /Applications (install rekordbox 7 from rekordbox.com first)");
        say("fail", "rekordbox isn't installed. Install rekordbox 7 from rekordbox.com, then press Check again.");
        t.mods = modChecks({}, true);
        return t;
    }
    t.found = true;
    t.app = apps[0];
    NSDictionary* info = [NSDictionary dictionaryWithContentsOfFile:ns(t.app + "/Contents/Info.plist")];
    t.version = str(info[@"CFBundleShortVersionString"]);  // 7.2.19.0342 -> 7.2.19, as on Windows
    if (std::count(t.version.begin(), t.version.end(), '.') >= 3) t.version = t.version.substr(0, t.version.rfind('.'));
    add("ok", "rekordbox " + t.version, t.app + (apps.size() > 1 ? " (the newest of " + std::to_string(apps.size()) + ")" : ""));

    std::vector<uint8_t> bin;
    if (!readAll(t.app + "/Contents/MacOS/rekordbox", &bin)) {
        t.problem = "rekordbox's program file couldn't be read.";
        add("fail", "Program", t.problem);
        say("fail", t.problem);
        t.mods = modChecks({}, true);
        return t;
    }
    static const char kMarker[] = "RBMHOOKS";
    if (memmem(bin.data(), bin.size(), kMarker, 8)) {
        t.problem = "This rekordbox is already patched. Reinstall rekordbox from rekordbox.com and try again.";
        add("fail", "Program", "already patched (not an original rekordbox)");
        say("fail", "This rekordbox has been changed. Reinstall it from rekordbox.com, then press Check again.");
        t.mods = modChecks({}, true);
        return t;
    }
    macho::Result r = macho::check(bin);
    if (!r.error.empty()) {
        t.problem = "rekordbox's program can't be patched: " + r.error + ".";
        add("fail", "Program", r.error);
        say("fail", "This rekordbox can't be patched (" + r.error + ").");
        t.mods = modChecks({}, true);
        return t;
    }
    t.build = r.uuid;
    t.tested = r.uuid == kTestedUuid;
    if (t.tested) add("ok", "Build", "the build platendoos was tested with (" + r.uuid.substr(0, 8) + "…)");
    else if (r.uuid == kCheckedUuid) add("ok", "Build", "rekordbox 7.2.20: every mod was run on it (" + r.uuid.substr(0, 8) + "…)");
    else {
        add("warn", "Build", "not the tested build (7.2.19): the mods find rekordbox's code by name, mods whose code changed switch themselves off");
        say("warn", "platendoos hasn't been tested with this rekordbox version yet. It should work: a mod that doesn't fit switches itself off.");
    }

    int total = macho::kHookCount;
    std::string sites = std::to_string(r.patched) + " of " + std::to_string(total) + " found";
    if (r.patched == total) add("ok", "Hook sites", sites);
    else {
        std::string miss;
        for (size_t i = 0; i < r.skipped.size() && i < 3; i++) miss += (i ? "; " : "") + r.skipped[i];
        if (r.skipped.size() > 3) miss += "; …";
        add(r.patched ? "warn" : "fail", "Hook sites", sites + ": " + miss);
        if (!r.patched) say("fail", "This rekordbox version is too different for platendoos.");
    }
    // The Intel (x86_64) code has its own sites: mentioned when some are missing, and what an Intel Mac's mods go by.
    const macho::ArchResult* x86 = r.arch(macho::kX86_64);
    if (std::string sites86 = intelSites(r); !sites86.empty())
        add(x86->patched ? "warn" : "fail", "Hook sites (Intel)", sites86);
    bool intel = intelMac();
    if (intel && !x86) {
        add("fail", "Program", "no Intel (x86_64) code");
        say("fail", "This rekordbox has no code for Intel Macs.");
    }
    if (intel && x86) {  // (new in 0.5.0: tested under Rosetta only)
        add("warn", "Intel Mac", "beta: platendoos has not been tested on a real Intel Mac yet");
        say("warn", "Intel Macs are new (beta): tested only under Rosetta so far. If something goes wrong, the log in "
                    "~/Library/Logs/rekordbox-mods.log helps; plain rekordbox stays as it was.");
    }
    t.mods = modChecks(intel && x86 ? x86->found : r.found, true);

    if (!payOk) add("fail", "Mods", "the patcher's own files are missing (" + pay + ")"), say("fail", "The patcher is incomplete. Download it again.");
    else add("ok", "Mods", "librbmods.dylib, models and windows (" + humanSize((double)paySize) + ")");

    NSDictionary* fs = [fm attributesOfFileSystemForPath:NSHomeDirectory() error:nil];
    uint64_t freeBytes = [fs[NSFileSystemFreeSize] unsignedLongLongValue];
    uint64_t need = paySize + bin.size() * 2 + (200ull << 20);  // the app is cloned (APFS: shares rekordbox's blocks)
    if (t.installed) need += paySize + bin.size();  // the old one stays until the new one is complete
    if (freeBytes && freeBytes < need) {
        add("fail", "Disk space", humanSize((double)freeBytes) + " free, needs about " + humanSize((double)need));
        say("fail", "Not enough disk space: about " + humanSize((double)need) + " is needed, " + humanSize((double)freeBytes) + " is free.");
    }
    else add("ok", "Disk space", humanSize((double)freeBytes) + " free");

    if (t.installed && runningAt(t.dest)) say("warn", "platendoos is open: it will be closed while it's updated.");
    if (t.installed) add(runningAt(t.dest) ? "warn" : "ok", "platendoos",
                         runningAt(t.dest) ? "installed and running: it will be quit and replaced" : "installed: it will be replaced");

    if (!payOk) t.problem = "The patcher is incomplete.";
    else if (intel && !x86) t.problem = "This rekordbox has no code for Intel Macs.";
    else if (r.patched == 0 || (intel && !x86->patched)) t.problem = "This rekordbox version is too different.";
    else if (freeBytes && freeBytes < need) t.problem = "Not enough disk space.";
    t.supported = t.problem.empty();
    return t;
}

namespace {

// Whether path is a platendoos app this patcher may delete: a copy of rekordbox's app (PLATENDOOS_DEST may name
// anything), never rekordbox's own.
bool replaceable(NSString* path, const Target& t) {
    return [path hasSuffix:@".app"] && [NSFileManager.defaultManager fileExistsAtPath:[path stringByAppendingPathComponent:@"Contents/MacOS/rekordbox"]] &&
           ![[path stringByStandardizingPath] isEqualToString:[ns(t.app) stringByStandardizingPath]];
}

// Made from scratch beside the old app (same folder, so the swap is a rename on one volume), which is only replaced
// once the new one is complete and signed: a failure leaves the old one as it was.
std::string build(const Target& t, const Options& o, const Step& step, NSString* dest, const double N) {
    NSFileManager* fm = NSFileManager.defaultManager;
    std::string out;
    step("Copying " + t.app, 1 / N);
    if (copyfile(t.app.c_str(), dest.UTF8String, nullptr, COPYFILE_ALL | COPYFILE_RECURSIVE | COPYFILE_CLONE) != 0)
        return "rekordbox couldn't be copied: " + std::string(strerror(errno));

    step("Patching rekordbox's program to load the mods", 2 / N);
    std::string binPath = str(dest) + "/Contents/MacOS/rekordbox";
    std::vector<uint8_t> bin;
    if (!readAll(binPath, &bin)) return "The copied program couldn't be read.";
    macho::Result r = macho::patch(bin);
    if (!r.error.empty()) return "Patching failed: " + r.error;
    if (!writeAll(binPath + ".new", bin.data(), bin.size()) || rename((binPath + ".new").c_str(), binPath.c_str()) != 0)
        return "The patched program couldn't be written.";
    chmod(binPath.c_str(), 0755);
    std::string intel = intelSites(r);
    step("  " + std::to_string(r.patched) + " of " + std::to_string(macho::kHookCount) + " hooks in place" +
         (intel.empty() ? "" : " (Intel code: " + intel + ")"), 3 / N);

    step("Adding the mods, their models and windows", 3.5 / N);
    std::string pay = payload();
    NSString* res = [dest stringByAppendingPathComponent:@"Contents/Resources/rbmods"];
    [fm removeItemAtPath:res error:nil];
    // (no COPYFILE_XATTR: no quarantine flag on what is copied from a downloaded patcher)
    int flags = COPYFILE_DATA | COPYFILE_STAT | COPYFILE_RECURSIVE;
    if (copyfile((pay + "/rbmods").c_str(), res.UTF8String, nullptr, flags) != 0 ||
        copyfile((pay + "/librbmods.dylib").c_str(), (str(dest) + "/Contents/MacOS/librbmods.dylib").c_str(), nullptr, COPYFILE_DATA | COPYFILE_STAT) != 0)
        return "The mods couldn't be copied: " + std::string(strerror(errno));

    if (o.flx4) {
        step("Adding the DDJ-FLX4 mapping (four decks, stems)", 5 / N);
        std::string csv = str(dest) + "/Contents/Resources/MidiMappings/DDJ-FLX4.midi.csv";
        std::vector<uint8_t> src;
        std::string why;
        std::string m = readAll(t.app + "/Contents/Resources/MidiMappings/DDJ-FLX4.midi.csv", &src)
                            ? flx4map::make(std::string(src.begin(), src.end()), &why)
                            : (why = "rekordbox has no DDJ-FLX4 mapping", "");
        if (m.empty()) step("  skipped: " + why, 5.5 / N);
        else if (!writeAll(csv, m.data(), m.size())) step("  skipped: couldn't write the mapping", 5.5 / N);
    }

    step("Giving it its own icon", 6 / N);
    NSString* plist = [dest stringByAppendingPathComponent:@"Contents/Info.plist"];
    NSMutableDictionary* info = [NSMutableDictionary dictionaryWithContentsOfFile:plist];
    std::string why;
    NSString* resDir = [dest stringByAppendingPathComponent:@"Contents/Resources"];
    if (info && invertIcon([resDir stringByAppendingPathComponent:@"rekordbox.icns"], [resDir stringByAppendingPathComponent:@"platendoos.icns"], &why)) {
        // CFBundleName stays rekordbox: rekordboxAgent refuses any other
        info[@"CFBundleIconFile"] = @"platendoos.icns";
        [info removeObjectForKey:@"CFBundleIconName"];  // else macOS takes Assets.car's (rekordbox's) icon
        if (![info writeToFile:plist atomically:YES]) step("  the icon couldn't be set", 6.5 / N);
    } else {
        step("  kept rekordbox's icon (" + (why.empty() ? std::string("no icon") : why) + ")", 6.5 / N);
    }

    step("Signing it for this Mac", 7 / N);
    NSString* ents = [NSTemporaryDirectory() stringByAppendingPathComponent:[NSString stringWithFormat:@"platendoos-ents-%d.plist", getpid()]];
    [ns(kEntitlements) writeToFile:ents atomically:YES encoding:NSUTF8StringEncoding error:nil];
    if (run(@"/usr/bin/codesign", @[ @"--force", @"--sign", @"-", [dest stringByAppendingPathComponent:@"Contents/MacOS/librbmods.dylib"] ], &out) != 0) {
        [fm removeItemAtPath:ents error:nil];
        return "Signing the mods failed: " + out;
    }
    int signedApp = run(@"/usr/bin/codesign", @[ @"--force", @"--sign", @"-", @"--options", @"runtime", @"--entitlements", ents, ns(binPath) ], &out);
    [fm removeItemAtPath:ents error:nil];
    if (signedApp != 0) return "Signing failed: " + out;
    // (the signature doesn't depend on where the app is: it still holds after the rename)
    step("Checking the signature", 8 / N);
    if (run(@"/usr/bin/codesign", @[ @"--verify", @"--strict", dest ], &out) != 0) return "The signature doesn't check out: " + out;
    run(@"/usr/bin/xattr", @[ @"-dr", @"com.apple.quarantine", dest ]);
    return "";
}

}  // namespace

std::string install(const Target& t, const Options& o, const Step& step) {
    NSFileManager* fm = NSFileManager.defaultManager;
    NSString* dest = ns(t.dest);
    NSError* err = nil;
    const double N = 9;

    // What is there is only replaced if it is ours: checked before anything is made
    bool had = [fm fileExistsAtPath:dest];
    if (had && !replaceable(dest, t))
        return t.dest + " is not a platendoos app: it is not replaced. Remove it yourself, or install somewhere else.";
    NSString* dir = [dest stringByDeletingLastPathComponent];
    NSString* name = dest.lastPathComponent.stringByDeletingPathExtension;
    NSString* tmp = [dir stringByAppendingPathComponent:[NSString stringWithFormat:@".%@-new.app", name]];
    NSString* old = [dir stringByAppendingPathComponent:[NSString stringWithFormat:@".%@-old.app", name]];
    [fm createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
    // leftovers of an install that was cut off (only ours)
    for (NSString* p in @[ tmp, old ])
        if ([fm fileExistsAtPath:p]) {
            if (!replaceable(p, t)) return str(p) + " is in the way: remove it, then install again.";
            [fm removeItemAtPath:p error:nil];
        }

    std::string e = build(t, o, step, tmp, N);
    if (!e.empty()) {
        [fm removeItemAtPath:tmp error:nil];
        return e + (had ? " (the installed platendoos is unchanged)" : "");
    }

    step("Quitting platendoos", 8.5 / N);
    bool wasRunning = runningAt(t.dest);
    run(@"/bin/zsh", @[ @"-c", ns(kQuitScript), @"quit", dest ]);
    if (wasRunning) g_quitAt = CFAbsoluteTimeGetCurrent();
    step("Putting the new platendoos in place", 8.7 / N);
    if (!had) {
        if (rename(tmp.UTF8String, dest.UTF8String) != 0) {
            std::string why = strerror(errno);
            [fm removeItemAtPath:tmp error:nil];
            return "The new platendoos couldn't be put in place: " + why;
        }
    } else if (renamex_np(tmp.UTF8String, dest.UTF8String, RENAME_SWAP) == 0) {
        // swapped in one step: tmp now holds the old app
        if (![fm removeItemAtPath:tmp error:&err]) step("  the old platendoos couldn't be removed: " + str(err.localizedDescription) + " (" + str(tmp) + ")", 8.9 / N);
    } else {
        // (a volume without swap) the old one aside, the new one in, the old one back if that fails
        if (rename(dest.UTF8String, old.UTF8String) != 0) {
            std::string why = strerror(errno);
            [fm removeItemAtPath:tmp error:nil];
            return "The old platendoos couldn't be replaced: " + why + " (it is unchanged)";
        }
        if (rename(tmp.UTF8String, dest.UTF8String) != 0) {
            std::string why = strerror(errno);
            rename(old.UTF8String, dest.UTF8String);
            [fm removeItemAtPath:tmp error:nil];
            return "The new platendoos couldn't be put in place: " + why + " (the old one is unchanged)";
        }
        if (![fm removeItemAtPath:old error:&err]) step("  the old platendoos couldn't be removed: " + str(err.localizedDescription) + " (" + str(old) + ")", 8.9 / N);
    }
    // Finder and the Dock pick up the new icon
    [NSWorkspace.sharedWorkspace noteFileSystemChanged:dest];
    run(@"/usr/bin/touch", @[ dest ]);
    // The new copy is a new app to macOS (its signature is made here), and rekordbox opens the audio device's inputs:
    // macOS asks about the microphone at its first start, and rekordbox's start stands still until that is answered.
    step("At its first start macOS asks whether rekordbox may use the microphone: allow it (unanswered for 2 minutes, rekordbox starts without sound)", 0.99);
    step("Done: " + t.dest, 1);
    return "";
}

std::string uninstall(const Target& t, const Step& step) {
    step("Quitting platendoos", 0.3);
    run(@"/bin/zsh", @[ @"-c", ns(kQuitScript), @"quit", ns(t.dest) ]);
    step("Moving " + t.dest + " to the Trash", 0.6);
    NSError* err = nil;
    if (![NSFileManager.defaultManager trashItemAtURL:[NSURL fileURLWithPath:ns(t.dest)] resultingItemURL:nil error:&err])
        return "It couldn't be removed: " + str(err.localizedDescription);
    step("Removed. Your rekordbox library, settings and the mods' caches are untouched.", 1);
    return "";
}

// Not straight after the old one was quit: a few seconds for it and its rekordboxAgent to be gone for good (install.sh
// does the same).
void launch(const Target& t) {
    NSURL* url = [NSURL fileURLWithPath:ns(t.dest)];
    double wait = std::max(0.0, 6 - (CFAbsoluteTimeGetCurrent() - g_quitAt));
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(wait * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        [NSWorkspace.sharedWorkspace openApplicationAtURL:url
                                            configuration:[NSWorkspaceOpenConfiguration configuration]
                                        completionHandler:nil];
    });
}

void reveal(const std::string& path) {
    [NSWorkspace.sharedWorkspace activateFileViewerSelectingURLs:@[ [NSURL fileURLWithPath:ns(path)] ]];
}

}  // namespace patcher

// ---- the app ----------------------------------------------------------------------------------------------

@interface PatcherDelegate : NSObject <NSApplicationDelegate>
@end
@implementation PatcherDelegate
- (void)applicationDidFinishLaunching:(NSNotification*)n {
    patcher::start();
    [NSApp activateIgnoringOtherApps:YES];
    // quit once its window is closed
    [NSTimer scheduledTimerWithTimeInterval:0.5 repeats:YES block:^(NSTimer*) {
        if (!patcher::windowOpen() && !patcher::busy()) [NSApp terminate:nil];
    }];
}
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)app {
    return patcher::busy() ? NSTerminateCancel : NSTerminateNow;
}
@end

int main(int argc, const char** argv) {
    @autoreleasepool {
        if (argc > 1 && !strncmp(argv[1], "--", 2)) return patcher::cli(argv[1]);
        NSApplication* app = NSApplication.sharedApplication;
        app.activationPolicy = NSApplicationActivationPolicyRegular;
        NSMenu* bar = [NSMenu new];
        NSMenuItem* appItem = [bar addItemWithTitle:@"" action:nil keyEquivalent:@""];
        NSMenu* appMenu = [NSMenu new];
        [appMenu addItemWithTitle:@"Quit platendoos patcher" action:@selector(terminate:) keyEquivalent:@"q"];
        appItem.submenu = appMenu;
        NSMenuItem* editItem = [bar addItemWithTitle:@"" action:nil keyEquivalent:@""];
        NSMenu* edit = [[NSMenu alloc] initWithTitle:@"Edit"];
        [edit addItemWithTitle:@"Copy" action:@selector(copy:) keyEquivalent:@"c"];
        [edit addItemWithTitle:@"Select All" action:@selector(selectAll:) keyEquivalent:@"a"];
        editItem.submenu = edit;
        NSMenu* win = [[NSMenu alloc] initWithTitle:@"Window"];
        [win addItemWithTitle:@"Close" action:@selector(performClose:) keyEquivalent:@"w"];
        [bar addItemWithTitle:@"" action:nil keyEquivalent:@""].submenu = win;
        app.mainMenu = bar;
        PatcherDelegate* d = [PatcherDelegate new];
        app.delegate = d;
        [app run];
    }
    return 0;
}
