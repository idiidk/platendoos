#!/bin/zsh
# Publishes a GitHub release of platendoos: builds both patchers at HEAD and uploads them with gh.
#   tools/build/release.sh <version> [--draft] [--inputs] [--edition public|full]   e.g.  tools/build/release.sh 1.0.0
# Only the public edition is ever published (docs/editions.md); it is the default.
# -> release v<version> with platendoos-patcher-mac-<version>.zip, platendoos-windows-<version>.zip, SHA256SUMS.txt
#
# Needs: gh (logged in), a clean tree pushed to GitHub, xwin for the Windows build (see tools/win/build.sh), and the
# build inputs that aren't committed: the models in models/ and the Windows tables and signatures in tools/win/
# (tools/build/fetch_inputs.sh downloads the last uploaded ones; tools/models/ and tools/win/ make new ones).
# --inputs also uploads those inputs (platendoos-build-inputs.zip) to the "build-inputs" release, replacing what's
# there, so fetch_inputs.sh gets them: do that whenever a model, table or signature file changed.
# --draft makes a draft release, to check on GitHub before publishing it.
# The notes are the version's section of CHANGELOG.md ("## <version> (date)"); without one, the commits since the last release.
# Nothing from rekordbox goes into a release except the Windows signatures/tables (byte patterns and addresses).
set -euo pipefail
root=${0:A:h:h:h}
cd $root

ver= draft=() inputs=0 edition=public want=
for a in "$@"; do
    if [[ $want == edition ]]; then edition=$a want=; continue; fi
    case $a in
        --draft) draft=(--draft) ;;
        --inputs) inputs=1 ;;
        --edition) want=edition ;;
        --edition=*) edition=${a#--edition=} ;;
        -*) echo "unknown option $a"; exit 1 ;;
        *) ver=$a ;;
    esac
done
[[ -n $ver && -z $want ]] || { sed -n '2,/^set -euo/p' $0 | sed '$d' | grep -v 'private>' | sed 's/^# \{0,1\}//'; exit 1; }
[[ $edition == public || $edition == full ]] || { echo "unknown edition '$edition' (public or full)"; exit 1; }
[[ $edition == full && ! -d $root/private ]] && { echo "the full edition needs private/ (this is the public tree)"; exit 1; }
export RBM_EDITION=$edition
[[ $ver =~ '^[0-9A-Za-z][0-9A-Za-z.+-]*$' ]] || { echo "bad version '$ver' (e.g. 1.0.0, 1.1.0-beta1)"; exit 1; }
tag=v$ver


command -v gh >/dev/null || { echo "needs the GitHub CLI: brew install gh; gh auth login"; exit 1; }
[[ -z $(git status --porcelain) ]] || { echo "uncommitted changes: commit (or stash) them first, a release is built from HEAD"; git status --short; exit 1; }
sha=$(git rev-parse HEAD)
git fetch -q origin
[[ -n $(git branch -r --contains $sha) ]] || { echo "HEAD ($sha) isn't on GitHub yet: push it first"; exit 1; }
gh release view $tag >/dev/null 2>&1 && { echo "release $tag exists already"; exit 1; }

# the inputs every package needs
missing=()
for f in models/beatthis.mlmodelc models/htdemucs.mlmodelc models/effnet.mlmodelc models/keycnn.bin \
         models/beatthis.onnx models/htdemucs.onnx models/effnet.onnx; do
    [[ -e $f ]] || missing+=$f
done
(( $(ls tools/win/tables/*.tbl 2>/dev/null | wc -l) )) || missing+="tools/win/tables/*.tbl"
(( $(ls tools/win/sigs/*.sig 2>/dev/null | wc -l) )) || missing+="tools/win/sigs/*.sig"
(( ${#missing} == 0 )) || { echo "missing build inputs: ${missing[*]}"; echo "run tools/build/fetch_inputs.sh, or make them (models/README.md)"; exit 1; }
# (tables and signatures made for this edition only: tools/win/checkinputs.py)
python3 -I tools/win/checkinputs.py $root tools/win/tables/*.tbl tools/win/sigs/*.sig || exit 1

dist=$root/build/release/$tag
rm -rf $dist; mkdir -p $dist

if (( inputs )); then
    echo "== build inputs"
    (zip -qr $dist/platendoos-build-inputs.zip models tools/win/tables tools/win/sigs -x 'models/README.md' '*.DS_Store')
    if ! gh release view build-inputs >/dev/null 2>&1; then
        gh release create build-inputs --target $sha --prerelease --latest=false --title "Build inputs (not a release)" \
            --notes "The models and Windows tables/signatures that the build needs and that aren't committed. tools/build/fetch_inputs.sh downloads them. To install platendoos, take the latest release instead. Model licences: NOTICE.md."
    fi
    gh release upload build-inputs $dist/platendoos-build-inputs.zip --clobber
    rm $dist/platendoos-build-inputs.zip
fi

echo "== macOS patcher (public edition)"
PLATENDOOS_VERSION=$ver tools/build/patcher_mac.sh
cp build/platendoos-patcher-mac.zip $dist/platendoos-patcher-mac-$ver.zip
echo "== Windows (public edition)"
PLATENDOOS_VERSION=$ver tools/win/build.sh
cp build/win/platendoos-windows.zip $dist/platendoos-windows-$ver.zip
(cd $dist && shasum -a 256 *.zip > SHA256SUMS.txt)

prev=$(git describe --tags --abbrev=0 --match 'v*' $sha^ 2>/dev/null || true)
{
    print -r -- "## Install"
    print
    print -r -- "Needs rekordbox 7 installed (made for 7.2.19 and 7.2.20, tested on both). The patcher carries only the mods and their models, no part of rekordbox."
    print
    print -r -- "- **macOS** (Apple Silicon; **Intel Macs in beta**, tested only under Rosetta so far): unzip \`platendoos-patcher-mac-$ver.zip\` and open **platendoos patcher**. It isn't notarised: the first time, open *System Settings > Privacy & Security* and choose *Open Anyway*."
    print -r -- "- **Windows** (x64): unzip \`platendoos-windows-$ver.zip\` anywhere and run **platendoos patcher.exe** in the \`platendoos\` folder. It isn't signed: SmartScreen may warn, *More info > Run anyway*."
    print
    # the version's section of CHANGELOG.md (from "## <ver>" up to the next "## "), its ### headings one level up
    # (without the full edition's private blocks and lines: tools/build/strip_private.py)
    changes=$(awk -v v="$ver" '/^## / { on = (index($0, "## " v " ") == 1 || $0 == "## " v); next } on' CHANGELOG.md 2>/dev/null \
              | python3 -I tools/build/strip_private.py - \
              | sed 's/^### /## /' | sed '/./,$!d' \
              | awk '/^  [^ ]/ && prev != "" { sub(/^ +/, ""); line = line " " $0; next } { if (NR > 1) print line; line = $0; prev = $0 } END { print line }' \
              | sed -E "s#\]\((docs/[^)]*|[A-Z]+\.md)\)#](https://github.com/idiidk/platendoos/blob/$tag/\1)#g")
    if [[ -n $changes ]]; then
        print -r -- "# What's new in $ver"
        print
        print -r -- "$changes"
        print
    elif [[ -n $prev ]]; then
        print -r -- "## Changes since $prev"
        print
        git log --no-merges --format='- %s' $prev..$sha
        print
    fi
    print -r -- "## Licence"
    print
    print -r -- "CC BY-NC-SA 4.0: non-commercial use only. The models keep their own licences (madmom's key CNN and Discogs-EffNet are CC BY-NC-SA 4.0 as well, Beat This! and HTDemucs MIT): see NOTICE.md, also in the packages."
} > $dist/notes.md

echo "== release $tag"
gh release create $tag --target $sha --title "platendoos $ver" --notes-file $dist/notes.md $draft \
    $dist/platendoos-patcher-mac-$ver.zip $dist/platendoos-windows-$ver.zip $dist/SHA256SUMS.txt
gh release view $tag --json url -q .url
