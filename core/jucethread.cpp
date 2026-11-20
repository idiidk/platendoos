// Lets the mods' own threads call rekordbox code that asks juce::Thread::getCurrentThread() for the thread it runs on.
//
// On a thread juce didn't start (std::thread, the mods' workers) getCurrentThread() is null. rekordbox mostly checks
// for that, but 7.2.20's streaming::DownloadManager::syncDownload doesn't: after a Spotify track's download it waits
// up to 30 s for the track to become readable, calling getCurrentThread()->threadShouldExit() unguarded. A Spotify
// reader opened on one of the mods' threads (reading a whole Spotify track in the background) crashed rekordbox there (a read at
// 0x1ad, threadShouldExit's flag).
//
// juce keeps the current thread in a ThreadLocalValue<Thread*> of its CurrentThreadHolder: a list of {thread id,
// next, value}. JuceThreadScope gives the calling thread an entry (getCurrentThread() makes one, null) and points it
// at a zeroed stand-in juce::Thread for as long as the scope lasts: threadShouldExit() reads false from it, as if the
// thread had never been asked to stop, and its name ("platendoos worker") is what fileOpen reads. Nothing else of a
// juce::Thread is used on these paths; the entry is set
// back to null afterwards, so code that checks for "no juce thread" sees that again.
// Its addresses belong to the pseudo-mod "jucethread": where one doesn't resolve, the scope does nothing.
#include "rbmod.h"

#include <atomic>
#ifdef _WIN32
#include <windows.h>
#endif

using namespace rbm;

static const Addr A_getCurrentThread{"__ZN4juce6Thread16getCurrentThreadEv", intptr_t(0), "jucethread"};
#ifndef _WIN32  // Windows: inlined everywhere, (ThreadID) GetCurrentThreadId() zero-extended (ThreadLocalValue::get)
static const Addr A_getCurrentThreadId{"__ZN4juce6Thread18getCurrentThreadIdEv", intptr_t(0), "jucethread"};
#endif
static const Addr A_getCurrentThreadHolder{"__ZN4juceL22getCurrentThreadHolderEv", intptr_t(0), "jucethread"};
// What rekordbox reads of the current juce::Thread on these paths: its name (StreamingReader::fileOpen checks whether
// it is an analysis thread) and the flag threadShouldExit returns. Windows 7.2.20: 0x8 and 0x1ed (table lines).
static const Offset O_threadName{0x8, "__ZN6rbxfrm15StreamingReader8fileOpenERKN4juce6StringE", "add x0", 0, "jucethread"};
static const Offset O_shouldExit{0x1ad, "__ZNK4juce6Thread16threadShouldExitEv", "add x0", 0, "jucethread"};

namespace {
struct ObjectHolder {  // juce::ThreadLocalValue<Thread*>::ObjectHolder
    std::atomic<void*> threadId;
    ObjectHolder* next;
    void* value;
};
// The stand-in: zeroed, larger than a juce::Thread, with a name (a juce::String, its text pointer) and the stop flag
// clear.
alignas(16) unsigned char g_standIn[4096];

bool standInReady() {
    static int state;  // main-thread-safe enough: the first scope is made on one worker; a race writes the same values
    if (state) return state > 0;
    if (O_threadName + sizeof(void*) > sizeof g_standIn || O_shouldExit >= sizeof g_standIn) {
        state = -1;
        return false;
    }
    static JStr name("platendoos worker");  // kept for good: the stand-in's name points at its text
    memcpy(g_standIn + O_threadName, &name, sizeof(void*));
    state = 1;
    return true;
}

ObjectHolder* entryOfThisThread() {
    if (versionOff("jucethread")) return nullptr;
    // A ReferenceCountedObjectPtr<CurrentThreadHolder>, returned through the pointer in the first argument (x0;
    // rcx on Windows): {vptr, int refcount, ThreadLocalValue::first}. The reference it took is given back (juce's
    // own static pointer keeps the holder).
    void* holder = nullptr;
    at<void (*)(void**)>(A_getCurrentThreadHolder)(&holder);
    if (!holder) return nullptr;
#ifdef _WIN32
    void* id = (void*)(uintptr_t)GetCurrentThreadId();
#else
    void* id = at<void* (*)()>(A_getCurrentThreadId)();
#endif
    ObjectHolder* found = nullptr;
    for (ObjectHolder* o = ((std::atomic<ObjectHolder*>*)((char*)holder + 0x10))->load(); o && !found; o = o->next)
        if (o->threadId.load() == id) found = o;
    ((std::atomic<int>*)((char*)holder + 8))->fetch_sub(1);
    return found;
}
}  // namespace

rbm::JuceThreadScope::JuceThreadScope() {
    if (versionOff("jucethread") || !standInReady()) return;
    if (at<void* (*)()>(A_getCurrentThread)()) return;  // a juce thread already (or one we set)
    ObjectHolder* o = entryOfThisThread();             // getCurrentThread() above made it
    if (!o || o->value) return;
    o->value = g_standIn;
    entry_ = o;
    static std::atomic<bool> said{false};
    if (!said.exchange(true)) logf("rbmods: the mods' threads stand in for juce threads where rekordbox needs one");
}

rbm::JuceThreadScope::~JuceThreadScope() {
    if (entry_) ((ObjectHolder*)entry_)->value = nullptr;
}
