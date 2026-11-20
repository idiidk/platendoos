#!/bin/zsh
# Downloads the build inputs that aren't committed (the models, and the Windows address tables and signatures made
# from rekordbox.exe) from the "build-inputs" GitHub release, which tools/build/release.sh --inputs uploads, and
# unpacks them into models/ and tools/win/{tables,sigs}/. Files already there are replaced.
#   tools/build/fetch_inputs.sh
set -euo pipefail
root=${0:A:h:h:h}
tmp=$(mktemp -d -t platendoos-inputs)
trap 'rm -rf $tmp' EXIT
zip=$tmp/platendoos-build-inputs.zip
if command -v gh >/dev/null && gh auth status >/dev/null 2>&1; then
    gh release download build-inputs -R idiidk/platendoos -p platendoos-build-inputs.zip -D $tmp
else
    curl -fL -o $zip https://github.com/idiidk/platendoos/releases/download/build-inputs/platendoos-build-inputs.zip
fi
# only the three folders it is meant to fill
unzip -q -o $zip 'models/*' 'tools/win/tables/*' 'tools/win/sigs/*' -d $root
echo "fetched into $root/models and $root/tools/win/{tables,sigs}:"
ls $root/models $root/tools/win/tables $root/tools/win/sigs
