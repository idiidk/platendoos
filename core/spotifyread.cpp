// Spotify readers that keep up with the download, for the mods that read a track while Spotify still downloads it
// (the analyzer's own runs, the stems separator, spotifyaudio's reads) or keep what the readers return.
//
// rekordbox reads a Spotify track through rbxfrm::SpotifyReader (a StreamingReader) around a SpotifyReadOgg. The
// SpotifyReadOgg decodes the part of the track its map covers: what was downloaded when the map was last made; past
// it, it returns silence. StreamingReader remakes the map as the download goes on (updateData) and when it is complete
// (completeData), and makes a read past the map wait for that. A read still gets past a map that is behind in two
// ways, and then returns silence for audio that is there:
//  - a reader opened on an analysis thread ignores both notices. rekordbox's own analysis only opens a track once
//    it is downloaded, so its map is whole from the start; the analyzer mod skips that wait for the tracks it queues,
//    and such a reader's map then stays at what was downloaded when it was opened. Once the download is complete
//    the reads are let through, and everything after the map is silence: the last 7-24 s of most tracks analysed
//    from the stream (their waveform's end was flat).
//  - StreamingReader::isReadyToRead sees a complete download by asking for it too, which can be just before
//    completeData has remade the map.
// So when a SpotifyReadOgg is asked for audio past its map, the map is remade first, the way completeData does it
// (updateMap). StreamingReader::fileRead only passes such a read on once the download is complete, and holds the
// reader's lock around it, as it does around its own map updates.
// Its recipes belong to the pseudo-mod "spotifyread": where one doesn't resolve, spotifyReadersKeepUp() is false.
#include "rbmod.h"

using namespace rbm;

static const Addr A_oggVtable{"__ZTVN6rbxfrm14SpotifyReadOggE", intptr_t(0), "spotifyread"};
static const Offset V_fileRead{0x20, "__ZTVN6rbxfrm14SpotifyReadOggE", "vt:__ZN6rbxfrm11FileReadOgg8fileReadEiPNS_6Float2Ei", 0, "spotifyread"};
static const Offset V_isReadyToRead{0x28, "__ZTVN6rbxfrm14SpotifyReadOggE", "vt:__ZN6rbxfrm14SpotifyReadOgg13isReadyToReadEii", 0, "spotifyread"};  // (pos, count): the map covers them
static const Offset V_updateMap{0xb0, "__ZTVN6rbxfrm14SpotifyReadOggE", "vt:__ZN6rbxfrm14SpotifyReadOgg9updateMapEv", 0, "spotifyread"};

typedef int (*ReadFn)(void*, int, void*, int);
static ReadFn o_read;

static int readOgg(void* self, int pos, void* out, int count) {
    void** vt = *(void***)self;
    if (count > 0 && !((bool (*)(void*, int, int))vt[V_isReadyToRead / 8])(self, pos, count))
        ((bool (*)(void*))vt[V_updateMap / 8])(self);  // (under a millisecond; a read past the track's real end does it again)
    return o_read(self, pos, out, count);
}

bool rbm::spotifyReadersKeepUp() {
    static int state;  // 0 not tried, 1 installed, -1 not possible
    if (state) return state > 0;
    state = -1;
    if (versionOff("spotifyread")) return false;
    void** vt = at<void**>(A_oggVtable) + 2;  // the object's vptr: __ZTV + 0x10
    o_read = (ReadFn)vt[V_fileRead / 8];
    if (!patchSlot(&vt[V_fileRead / 8], (void*)&readOgg)) {
        logf("rbmods: could not patch the SpotifyReadOgg vtable");
        return false;
    }
    state = 1;
    return true;
}
