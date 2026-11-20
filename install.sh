#!/bin/zsh
# Rebuilds librbmods.dylib and swaps it into an already built app (quits rekordbox first).
# Use build.sh instead after changing hooks.def (the binary patch depends on it).
#   ./install.sh [app]   default: ~/Applications/platendoos.app
#   RBM_SIGN_ID=<identity>  sign with this code-signing identity instead of ad hoc
#   RBM_RESIGN=1            sign the main executable again too (see below; macOS then asks about the microphone again)
#   RBM_NOOPEN=1            don't open the app afterwards
#   RBM_EDITION=public      install the public edition (docs/editions.md); default full
set -euo pipefail
here=${0:A:h}
source "$here/tools/build/edition.sh"
app=${1:-$HOME/Applications/platendoos.app}

$here/compile.sh

wasRunning=0
pgrep -f "^${app:A}/Contents/MacOS/rekordbox$" >/dev/null && wasRunning=1
$here/tools/build/quit.sh "$app"
quitAt=$SECONDS

mkdir -p "$app/Contents/Resources/rbmods"
cp -R "$here"/models/* "$app/Contents/Resources/rbmods/"
edition_ui "$app/Contents/Resources/rbmods"
cp "$here/build/librbmods.dylib" "$app/Contents/MacOS/"
id=${RBM_SIGN_ID:--}
codesign --force --sign "$id" "$app/Contents/MacOS/librbmods.dylib"
# The main executable keeps the signature it has. Its signature seals the bundle, so strictly it is stale once the
# dylib changes ("codesign --verify --strict" says nested code is modified), but macOS starts the app all the same,
# and to macOS it is still the same app: signing it again (ad hoc) makes it a new one with every build, and macOS then
# asks again whether rekordbox may use the microphone (it opens the audio device's inputs). Until that is answered
# rekordbox's start stands still in its audio start (CoreAudio waits for the answer), and after 2 minutes without one
# it goes on without sound: this was the "hang after a relaunch" (tccd's log: AUTHREQ_PROMPTING kTCCServiceMicrophone).
# It is signed again only when it has to be: it has no usable signature, its entitlements must change
# (RBM_DEBUGGABLE=1 lets a debugger attach, for development; back to 0 takes that away), or RBM_RESIGN=1.
bin="$app/Contents/MacOS/rekordbox"
has=$(codesign -d --entitlements - --xml "$bin" 2>/dev/null || true)
resign=${RBM_RESIGN:-0}
[[ $has == *disable-library-validation* ]] || resign=1
if [[ ${RBM_DEBUGGABLE:-0} == 1 ]]; then [[ $has == *get-task-allow* ]] || resign=1; else [[ $has == *get-task-allow* ]] && resign=1; fi
if [[ $resign == 1 ]]; then
    ents="$here/entitlements.plist"
    if [[ ${RBM_DEBUGGABLE:-0} == 1 ]]; then
        ents=$(mktemp -t rbm-ents).plist
        cp "$here/entitlements.plist" "$ents"
        plutil -insert "com\\.apple\\.security\\.get-task-allow" -bool true "$ents"
    fi
    codesign --force --sign "$id" --options runtime --entitlements "$ents" "$bin"
    codesign --verify --strict "$app"
    [[ $id == - ]] && echo "signed rekordbox anew: macOS will ask again whether it may use the microphone. Click Allow (within 2 minutes, or it starts without sound)"
fi
[[ ${RBM_NOOPEN:-0} == 1 ]] && exit 0
# Not straight after quitting: a few seconds for the old rekordbox and its agent to be gone for good.
(( wasRunning && SECONDS - quitAt < 6 )) && sleep $(( 6 - (SECONDS - quitAt) ))
open "$app"
