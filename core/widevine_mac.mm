// Keeps rekordbox from starting its Widevine library in platendoos: with an Apple Music account signed in, rekordbox
// crashed (or quit without a word) some ten seconds after launch.
//
// Apple Music's tracks are Widevine-protected, and rekordbox decrypts them with Castlabs' library (clatc.framework).
// rekordbox starts it when Apple Music logs in (db::StreamingBridge::initializeWidevineCdm, on a thread of that name)
// and again for every protected download; both end in streaming::CastlabsDecryptorDrmCdmRepository::InitializeCdm.
// The library's start first checks the signature of the app it runs in: the signature must be valid and carry the
// certificate it expects, or it calls exit(0). A patched rekordbox never passes:
//  - signed ad hoc by the patcher (valid, no certificates): the check reads the certificate list without looking
//    whether there is one, CFArrayGetCount(NULL): the crash (EXC_BAD_ACCESS in CF_IS_OBJC, thread
//    "initializeWidevineCdm");
//  - a signature that no longer matches the bundle (install.sh after build.sh), or another certificate: exit(0).
// Only Apple Music uses it, so without that account nothing ever got there.
//
// The check is the library's own protection and stays as it is. Instead the library is never started here:
// InitializeCdm first stores the keybox rekordbox's cloud hands out (writeKeyboxFile) and returns false when there is
// no keybox file (existsKeyboxFile), which is what it does on a Mac that never got one. The two hooks make that the
// answer every time: no keybox is fetched or written, the library's start isn't called, and rekordbox goes on as
// after any failed start: it logs it and Apple Music's tracks don't load (DecryptHeader / DecryptContent do nothing
// until a start has succeeded). Apple Music therefore doesn't play in platendoos; it does in plain rekordbox, which
// stays installed next to it.
// macOS only: there the patched binary is signed anew. On Windows rekordbox.exe stays as AlphaTheta signed it.
#include "rbmod.h"

#include <atomic>

using namespace rbm;

extern "C" void rbm_hook_widevine_keybox(void* /*keybox: the caller's unique_ptr, which the caller releases*/) {}

extern "C" bool rbm_hook_widevine_exists() {
    static std::atomic<bool> said{false};
    if (!said.exchange(true))
        logf("widevine: not started (its library only runs in rekordbox as AlphaTheta signed it, and quits any other): Apple Music's tracks "
             "don't play in platendoos, they do in plain rekordbox");
    return false;
}
