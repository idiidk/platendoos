// rbm::plat on macOS: GCD, Foundation/AppKit, mach.
#import <Security/Security.h>
#import <AppKit/AppKit.h>
#include "plat.h"
#include <CommonCrypto/CommonDigest.h>
#include <dlfcn.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <copyfile.h>
#include <fcntl.h>
#include <sys/clonefile.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <unistd.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <crt_externs.h>
#include <sys/wait.h>
#include <thread>
#include <vector>
#include <condition_variable>
#include <mutex>

namespace rbm::plat {

static void runBlock(void* ctx) {
    std::unique_ptr<std::function<void()>> fn((std::function<void()>*)ctx);
    (*fn)();
}
static void* box(std::function<void()> fn) { return new std::function<void()>(std::move(fn)); }

void onMain(std::function<void()> fn) { dispatch_async_f(dispatch_get_main_queue(), box(std::move(fn)), runBlock); }
void onMainAfter(double seconds, std::function<void()> fn) {
    dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(seconds * NSEC_PER_SEC)), dispatch_get_main_queue(),
                     box(std::move(fn)), runBlock);
}
void everyOnMain(double seconds, std::function<void()> fn) {
    dispatch_source_t t = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
    uint64_t ns = (uint64_t)(seconds * NSEC_PER_SEC);
    dispatch_source_set_timer(t, dispatch_time(DISPATCH_TIME_NOW, ns), ns, ns / 5);
    auto f = std::make_shared<std::function<void()>>(std::move(fn));
    dispatch_source_set_event_handler(t, ^{ (*f)(); });
    dispatch_resume(t);
    (void)CFBridgingRetain(t);  // kept for the app's lifetime
}
bool isMainThread() { return pthread_main_np() != 0; }
void background(std::function<void()> fn) {
    dispatch_async_f(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), box(std::move(fn)), runBlock);
}
void parallelFor(size_t n, const std::function<void(size_t)>& fn) {
    dispatch_apply(n, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^(size_t i) { fn(i); });
}

struct Queue::Impl { dispatch_queue_t q; };
Queue::Queue(const char* name) : impl(new Impl{dispatch_queue_create(name, DISPATCH_QUEUE_SERIAL)}) {}
Queue::~Queue() = default;
void Queue::async(std::function<void()> fn) { dispatch_async_f(impl->q, box(std::move(fn)), runBlock); }
void Queue::sync(const std::function<void()>& fn) {
    dispatch_sync(impl->q, ^{ fn(); });
}

double now() { return CFAbsoluteTimeGetCurrent(); }
double monotonic() {
    static mach_timebase_info_data_t tb = [] { mach_timebase_info_data_t t; mach_timebase_info(&t); return t; }();
    return (double)mach_absolute_time() * tb.numer / tb.denom / 1e9;
}
double idleSeconds() { return CGEventSourceSecondsSinceLastEventType(kCGEventSourceStateCombinedSessionState, kCGAnyInputEventType); }

std::filesystem::path fsPath(const std::string& s) { return std::filesystem::path(s); }
std::string utf8(const std::filesystem::path& p) { return p.string(); }
std::string homeDir() {
    const char* h = getenv("HOME");
    return h ? h : "";
}
std::string configDir() { return homeDir() + "/Library/Application Support/rekordbox-mods"; }
std::string logFile() { return homeDir() + "/Library/Logs/rekordbox-mods.log"; }
std::string cacheDir() { return homeDir() + "/Library/Caches/rekordbox-mods"; }
std::string resourcesDir() {
    @autoreleasepool {
        return std::string(NSBundle.mainBundle.resourcePath.UTF8String ?: "") + "/rbmods";
    }
}
FILE* openFile(const std::string& s, const char* mode) { return fopen(s.c_str(), mode); }
bool readFile(const std::string& s, std::string* out) {
    FILE* f = fopen(s.c_str(), "rb");
    if (!f) return false;
    out->clear();
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out->append(buf, n);
    bool ok = !ferror(f);  // (a drive unplugged mid-read: not the whole file)
    fclose(f);
    return ok;
}
bool writeFileAtomic(const std::string& s, const void* data, size_t n, bool flush) {
    std::string tmp = s + ".tmp" + std::to_string(getpid());
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, n, f) == n;
    // fsync, not F_FULLFSYNC: the data goes to the drive before the rename does, which is what an unplugged drive
    // needs; F_FULLFSYNC also makes the drive empty its whole write cache every time, far slower on a USB stick,
    // for what a checksum in the file can catch instead.
    if (flush && ok) ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
    ok = fclose(f) == 0 && ok;
    if (ok) ok = rename(tmp.c_str(), s.c_str()) == 0;
    if (!ok) unlink(tmp.c_str());
    return ok;
}
uint64_t diskFree(const std::string& s) {
    struct statfs st;
    return statfs(s.c_str(), &st) == 0 ? (uint64_t)st.f_bavail * st.f_bsize : 0;
}
bool externalVolume(const std::string& s) {
    @autoreleasepool {
        // the nearest folder that exists (a cache folder that is still to be made)
        NSString* path = @(s.c_str());
        while (path.length > 1 && ![NSFileManager.defaultManager fileExistsAtPath:path]) path = path.stringByDeletingLastPathComponent;
        NSURL* url = [NSURL fileURLWithPath:path];
        NSDictionary* v = [url resourceValuesForKeys:@[NSURLVolumeIsLocalKey, NSURLVolumeIsInternalKey] error:nil];
        NSNumber* local = v[NSURLVolumeIsLocalKey];
        NSNumber* internal = v[NSURLVolumeIsInternalKey];
        if (local && !local.boolValue) return true;  // a network share
        return internal && !internal.boolValue;     // (nil: not known, e.g. a disk image)
    }
}
void openPath(const std::string& s) {
    @autoreleasepool {
        [NSWorkspace.sharedWorkspace openURL:[NSURL fileURLWithPath:@(s.c_str())]];
    }
}
void openTextFile(const std::string& s) {
    @autoreleasepool {
        NSURL* file = [NSURL fileURLWithPath:@(s.c_str())];
        NSURL* app = [NSWorkspace.sharedWorkspace URLForApplicationWithBundleIdentifier:@"com.apple.TextEdit"];
        if (app)
            [NSWorkspace.sharedWorkspace openURLs:@[ file ] withApplicationAtURL:app
                                    configuration:[NSWorkspaceOpenConfiguration configuration] completionHandler:nil];
        else
            [NSWorkspace.sharedWorkspace activateFileViewerSelectingURLs:@[ file ]];
    }
}
std::string appVersion() {
    @autoreleasepool {
        NSString* v = NSBundle.mainBundle.infoDictionary[@"CFBundleShortVersionString"];
        return v ? v.UTF8String : "?";
    }
}
std::string nfc(const std::string& s) {
    @autoreleasepool {
        NSString* n = [NSString stringWithUTF8String:s.c_str()].precomposedStringWithCanonicalMapping;
        return n ? n.UTF8String : s;
    }
}
// (Both drain their own pool: a thread without one, e.g. one of rekordbox's, keeps what they autorelease until it ends.)
int httpPostJson(const std::string& url, const std::string& bearer, const std::string& json, std::string* body, double timeout) {
  @autoreleasepool {
    NSURL* u = [NSURL URLWithString:[NSString stringWithUTF8String:url.c_str()]];
    if (!u) return 0;
    NSURLSession* session = NSURLSession.sharedSession;
    NSMutableURLRequest* req = [NSMutableURLRequest requestWithURL:u];
    req.HTTPMethod = @"POST";
    req.timeoutInterval = timeout;
    req.HTTPBody = [NSData dataWithBytes:json.data() length:json.size()];
    [req setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];
    if (!bearer.empty())
        [req setValue:[@"Bearer " stringByAppendingString:[NSString stringWithUTF8String:bearer.c_str()]] forHTTPHeaderField:@"Authorization"];
    [req setValue:@"platendoos" forHTTPHeaderField:@"X-Title"];
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    __block int status = 0;
    __block NSData* data = nil;
    [[session dataTaskWithRequest:req completionHandler:^(NSData* d, NSURLResponse* resp, NSError* e) {
        if (!e && [resp isKindOfClass:NSHTTPURLResponse.class]) status = (int)((NSHTTPURLResponse*)resp).statusCode, data = d;
        dispatch_semaphore_signal(done);
    }] resume];
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
    if (body) body->assign(data ? (const char*)data.bytes : "", data ? data.length : 0);
    return status;
  }
}

static NSDictionary* secretQuery(const std::string& name) {
    return @{(__bridge id)kSecClass: (__bridge id)kSecClassGenericPassword,
             (__bridge id)kSecAttrService: @"platendoos",
             (__bridge id)kSecAttrAccount: [NSString stringWithUTF8String:name.c_str()]};
}
bool secretSet(const std::string& name, const std::string& value) {
    @autoreleasepool {
        NSDictionary* q = secretQuery(name);
        SecItemDelete((__bridge CFDictionaryRef)q);
        if (value.empty()) return true;
        NSMutableDictionary* add = [q mutableCopy];
        add[(__bridge id)kSecValueData] = [NSData dataWithBytes:value.data() length:value.size()];
        return SecItemAdd((__bridge CFDictionaryRef)add, nullptr) == errSecSuccess;
    }
}
std::string secretGet(const std::string& name) {
    @autoreleasepool {
        NSMutableDictionary* q = [secretQuery(name) mutableCopy];
        q[(__bridge id)kSecReturnData] = @YES;
        q[(__bridge id)kSecMatchLimit] = (__bridge id)kSecMatchLimitOne;
        CFTypeRef r = nullptr;
        if (SecItemCopyMatching((__bridge CFDictionaryRef)q, &r) != errSecSuccess || !r) return "";
        NSData* d = CFBridgingRelease(r);
        return std::string((const char*)d.bytes, d.length);
    }
}

int httpGet(const std::string& url, std::string* body, double timeout) {
    static NSURLSession* session = [] {
        NSURLSessionConfiguration* c = NSURLSessionConfiguration.ephemeralSessionConfiguration;
        return [NSURLSession sessionWithConfiguration:c];
    }();
  @autoreleasepool {
    NSURL* u = [NSURL URLWithString:[NSString stringWithUTF8String:url.c_str()]];
    if (!u) return 0;
    struct Result { std::mutex m; std::condition_variable cv; bool done = false; int status = 0; std::string data; };
    auto r = std::make_shared<Result>();  // (a late answer finds it still there)
    NSMutableURLRequest* req = [NSMutableURLRequest requestWithURL:u];
    req.timeoutInterval = timeout;
    [[session dataTaskWithRequest:req completionHandler:^(NSData* d, NSURLResponse* resp, NSError* e) {
        std::lock_guard<std::mutex> l(r->m);
        if (!e && [resp isKindOfClass:NSHTTPURLResponse.class]) {
            r->status = (int)((NSHTTPURLResponse*)resp).statusCode;
            if (d) r->data.assign((const char*)d.bytes, d.length);
        }
        r->done = true;
        r->cv.notify_all();
    }] resume];
    std::unique_lock<std::mutex> l(r->m);
    r->cv.wait_for(l, std::chrono::duration<double>(timeout + 5), [&] { return r->done; });
    if (!r->done) return 0;
    if (body) *body = std::move(r->data);
    return r->status;
  }
}
void revealPath(const std::string& s) {
    @autoreleasepool {
        [NSWorkspace.sharedWorkspace activateFileViewerSelectingURLs:@[ [NSURL fileURLWithPath:@(s.c_str())] ]];
    }
}

bool fileInfo(const std::string& s, FileInfo* out) {
    struct stat st;
    if (stat(s.c_str(), &st) != 0) return false;
    if (out) {
        out->size = (uint64_t)st.st_size;
        out->mtime = (double)st.st_mtimespec.tv_sec + st.st_mtimespec.tv_nsec / 1e9;
        out->isDir = S_ISDIR(st.st_mode);
    }
    return true;
}
bool setModified(const std::string& s, double mtime) {
    struct timespec ts[2];
    ts[0].tv_sec = ts[1].tv_sec = (time_t)mtime;
    ts[0].tv_nsec = ts[1].tv_nsec = (long)((mtime - (double)(time_t)mtime) * 1e9);
    return utimensat(AT_FDCWD, s.c_str(), ts, 0) == 0;
}

bool File::open(const std::string& s, bool create, bool readOnly) {
    close();
    int fd = ::open(s.c_str(), create ? O_RDWR | O_CREAT | O_TRUNC : readOnly ? O_RDONLY : O_RDWR, 0644);
    if (fd < 0) return false;
    h_ = fd;
    return true;
}
bool File::readAt(uint64_t offset, void* data, size_t n) { return h_ != -1 && pread((int)h_, data, n, (off_t)offset) == (ssize_t)n; }
bool File::writeAt(uint64_t offset, const void* data, size_t n) { return h_ != -1 && pwrite((int)h_, data, n, (off_t)offset) == (ssize_t)n; }
uint64_t File::size() {
    struct stat st;
    return h_ != -1 && fstat((int)h_, &st) == 0 ? (uint64_t)st.st_size : 0;
}
bool File::resize(uint64_t n) { return h_ != -1 && ftruncate((int)h_, (off_t)n) == 0; }
bool File::sync() { return h_ != -1 && fsync((int)h_) == 0; }
bool File::direct() { return h_ != -1 && fcntl((int)h_, F_NOCACHE, 1) == 0; }
void File::close() {
    if (h_ != -1) ::close((int)h_);
    h_ = -1;
}

bool copyFile(const std::string& from, const std::string& to) {
    if (clonefile(from.c_str(), to.c_str(), 0) == 0) return true;
    return copyfile(from.c_str(), to.c_str(), nullptr, COPYFILE_ALL) == 0;
}

int run(const std::vector<std::string>& argv, const std::string& cwd, const std::string& input, std::string* out, double timeout) {
    if (out) out->clear();
    if (argv.empty()) return -1;
    int in[2], outp[2];
    if (pipe(in)) return -1;
    if (pipe(outp)) { close(in[0]), close(in[1]); return -1; }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, in[0], 0);
    posix_spawn_file_actions_adddup2(&fa, outp[1], 1);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    for (int fd : {in[0], in[1], outp[0], outp[1]}) posix_spawn_file_actions_addclose(&fa, fd);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"  // (its successor needs macOS 26)
    if (!cwd.empty()) posix_spawn_file_actions_addchdir_np(&fa, cwd.c_str());
#pragma clang diagnostic pop
    std::vector<char*> args;
    for (const std::string& a : argv) args.push_back((char*)a.c_str());
    args.push_back(nullptr);
    // The child gets the three descriptors set up above and nothing else of rekordbox's. Without this it also got the
    // pipes of every other run() going on, and a program whose input pipe a sibling holds open never sees its end.
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT);
    pid_t pid = 0;
    int err = posix_spawn(&pid, argv[0].c_str(), &fa, &attr, args.data(), *_NSGetEnviron());
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&fa);
    close(in[0]), close(outp[1]);
    if (err) { close(in[1]), close(outp[0]); return -1; }
    std::thread writer([fd = in[1], input] {  // (its own thread: the program may not read until it has written)
        signal(SIGPIPE, SIG_IGN);
        size_t at = 0;
        while (at < input.size()) {
            ssize_t n = write(fd, input.data() + at, input.size() - at);
            if (n <= 0) break;
            at += (size_t)n;
        }
        close(fd);
    });
    double end = monotonic() + timeout;
    bool killed = false;
    char buf[65536];
    for (;;) {
        double left = end - monotonic();
        if (left <= 0) { kill(pid, SIGKILL), killed = true; break; }
        struct pollfd pf{outp[0], POLLIN, 0};
        int r = poll(&pf, 1, (int)std::min(left * 1000, 500.0));
        if (r < 0 && errno != EINTR) break;
        if (r <= 0) continue;
        ssize_t n = read(outp[0], buf, sizeof buf);
        if (n <= 0) break;
        if (out) out->append(buf, (size_t)n);
    }
    close(outp[0]);
    writer.join();
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return killed || !WIFEXITED(status) ? -1 : WEXITSTATUS(status);
}

// (not low is "utility": still below the interface and the audio, but on the fast cores and with unthrottled disk access)
void lowPriorityThread(bool low) { pthread_set_qos_class_self_np(low ? QOS_CLASS_BACKGROUND : QOS_CLASS_UTILITY, 0); }

bool patchMemory(void* addr, const void* bytes, size_t len) {
    vm_address_t page = (vm_address_t)addr & ~(vm_address_t)(vm_page_size - 1);
    vm_size_t size = ((vm_address_t)addr + len - page + vm_page_size - 1) & ~(vm_size_t)(vm_page_size - 1);
    // read-only data (vtables): writable copy-on-write, then back to read-only
    if (vm_protect(mach_task_self(), page, size, false, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) != KERN_SUCCESS) return false;
    memcpy(addr, bytes, len);
    vm_protect(mach_task_self(), page, size, false, VM_PROT_READ);
    return true;
}

std::string sha256Hex(const void* data, size_t n) {
    uint8_t d[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(data, (CC_LONG)n, d);
    char hex[2 * CC_SHA256_DIGEST_LENGTH + 1];
    for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; i++) snprintf(hex + 2 * i, 3, "%02x", d[i]);
    return hex;
}

void* loadLibraryFunction(const std::string& path, const char* name) {
    void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    return h ? dlsym(h, name) : nullptr;
}

}  // namespace rbm::plat
