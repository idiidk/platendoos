// rbm::plat: what the mods need from the operating system, so that core and mods are plain C++ shared by
// macOS (core/plat_mac.mm) and Windows (core/plat_win.cpp). Paths are UTF-8 std::strings with '/' separators
// on both (Windows accepts them); plat::fsPath() makes a std::filesystem::path of one.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace rbm::plat {

// ---- threads and time ---------------------------------------------------------------------

void onMain(std::function<void()> fn);                    // run on rekordbox's main (message) thread, later
void onMainAfter(double seconds, std::function<void()> fn);
void everyOnMain(double seconds, std::function<void()> fn);  // a repeating main-thread timer, for the app's lifetime
bool isMainThread();
void background(std::function<void()> fn);                // on a shared pool of worker threads
void parallelFor(size_t n, const std::function<void(size_t)>& fn);  // fn(0..n-1) on several threads; returns when done

// A serial queue: jobs run one after another on a worker thread, in order.
class Queue {
public:
    explicit Queue(const char* name);
    ~Queue();
    void async(std::function<void()> fn);
    void sync(const std::function<void()>& fn);  // runs fn on the queue and waits for it
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

double now();          // wall-clock seconds (only differences matter)
double monotonic();    // seconds since an arbitrary point, never jumps
double idleSeconds();  // since the user last used the keyboard or mouse

// ---- files --------------------------------------------------------------------------------

std::filesystem::path fsPath(const std::string& utf8);
std::string utf8(const std::filesystem::path& p);
std::string homeDir();
std::string configDir();    // the config file's folder: mac ~/Library/Application Support/rekordbox-mods, win %APPDATA%\rekordbox-mods
std::string logFile();      // mac ~/Library/Logs/rekordbox-mods.log, win %LOCALAPPDATA%\rekordbox-mods\rekordbox-mods.log
std::string cacheDir();     // mac ~/Library/Caches/rekordbox-mods, win %LOCALAPPDATA%\rekordbox-mods\cache
std::string resourcesDir(); // the models etc. that ship with the mods (mac: the app's Resources/rbmods)
FILE* openFile(const std::string& utf8, const char* mode);
bool readFile(const std::string& utf8, std::string* out);  // false if it can't be opened, or a read fails partway
// Via a temporary file and rename. flush: the data is on the drive before the rename (fsync / FlushFileBuffers), so a
// drive unplugged or a power cut leaves the old file or the whole new one; costs the time the drive takes to write it.
bool writeFileAtomic(const std::string& utf8, const void* data, size_t n, bool flush = false);
uint64_t diskFree(const std::string& utf8);   // bytes free on the volume holding the path (0 if unknown)
// Whether the path is on a drive outside the computer: USB (a stick, a card reader, an external disk), FireWire, an SD
// card or a network share. Such a drive may be slow or get unplugged. False for the internal disks, and when it can't
// be told. A path that doesn't exist yet is judged by its nearest folder that does. (Windows goes by the bus, so a
// Thunderbolt / USB4 NVMe enclosure or an eSATA disk counts as internal there; macOS knows those are external.)
bool externalVolume(const std::string& utf8);
void openPath(const std::string& utf8);       // opens a file or folder as if double-clicked
void revealPath(const std::string& utf8);     // shows it selected in Finder / Explorer
void openTextFile(const std::string& utf8);   // opens a text file (one without an extension too) in TextEdit / Notepad
std::string appVersion();                     // rekordbox's version ("7.2.19")
// Unicode NFC (precomposed): how rekordbox and the file system spell names with accents.
std::string nfc(const std::string& utf8);
// A GET request (any thread; blocks up to timeout seconds). The HTTP status (0 if there was no answer); body gets the
// response.
int httpGet(const std::string& url, std::string* body, double timeout = 20);
// A POST with a JSON body, bearer token optional (any thread; blocks). HTTPS only in practice; the status as above.
int httpPostJson(const std::string& url, const std::string& bearer, const std::string& json, std::string* body, double timeout = 60);
// A secret kept by the OS: macOS Keychain, Windows Credential Manager. Never in a file of ours. "" deletes it.
bool secretSet(const std::string& name, const std::string& value);
std::string secretGet(const std::string& name);  // "" if there is none

// Runs a program and waits for it (any thread; blocks up to timeout seconds, then kills it): argv[0] is its full path,
// `input` goes to its standard input, `out` gets its standard output (errors are dropped). No window, no shell
// (a .cmd / .bat on Windows goes through cmd.exe: keep such arguments free of quotes and %). The exit code, or -1 if
// it couldn't be started or was killed.
int run(const std::vector<std::string>& argv, const std::string& cwd, const std::string& input, std::string* out, double timeout = 60);

// ---- files, continued -----------------------------------------------------------------------

struct FileInfo {
    uint64_t size = 0;
    double mtime = 0;   // seconds since 1970
    bool isDir = false;
};
bool fileInfo(const std::string& utf8, FileInfo* out);   // false if the path doesn't exist (out may be null)
bool setModified(const std::string& utf8, double mtime);  // sets the file's modification (and access) time

// A file read and written at arbitrary offsets (pread / pwrite), opened read-write; for audio being recorded or tagged.
class File {
public:
    File() = default;
    ~File() { close(); }
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    bool open(const std::string& utf8, bool create, bool readOnly = false);  // create: made if missing and emptied; else it must exist
    bool isOpen() const { return h_ != -1; }
    bool readAt(uint64_t offset, void* data, size_t n);    // exactly n bytes, or false
    bool writeAt(uint64_t offset, const void* data, size_t n);  // exactly n bytes, or false
    uint64_t size();
    bool resize(uint64_t n);
    bool sync();   // flushed to the disk
    // What is written from now on goes straight to the drive instead of waiting in the system's cache (macOS
    // F_NOCACHE), so a writer that paces itself knows the drive has taken each piece. False where that can't be had
    // (Windows): sync() after each piece does it there.
    bool direct();
    void close();
private:
    intptr_t h_ = -1;  // fd / HANDLE
};

// Copies a file with its attributes, cloning it when the volume can (copy-on-write); the target should not exist.
bool copyFile(const std::string& from, const std::string& to);

void lowPriorityThread(bool low = true);  // the calling thread gets low CPU and disk priority (a background job); false: back to normal

// ---- rekordbox's process ------------------------------------------------------------------

// Makes len bytes at addr writable, writes them, restores the protection and flushes the instruction cache.
bool patchMemory(void* addr, const void* bytes, size_t len);
std::string sha256Hex(const void* data, size_t n);
#ifdef _WIN32
// A juce::CriticalSection of rekordbox's (a CRITICAL_SECTION on Windows), where juce's enter/exit are inlined.
void enterCriticalSection(void* cs);
void leaveCriticalSection(void* cs);
#endif

// ---- development --------------------------------------------------------------------------

// Loads a library and returns its function `name` (nullptr if either fails); for the hot-load experiments.
void* loadLibraryFunction(const std::string& path, const char* name);

}  // namespace rbm::plat
