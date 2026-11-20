#!/bin/bash
# deployT.sh: the Windows build in build/win/platendoos -> C:\rb\platendoosT on the PC (the test folder; not the user's install)
cd "$(dirname "$0")/../../.."
RBVM=rbpc tools/win/vm.sh stop
tmp=$(mktemp -d); (cd build/win && zip -qr "$tmp/p.zip" platendoos -x 'platendoos/rbmods/*.onnx')
scp -q -F ~/rbwin/vm/ssh_config "$tmp/p.zip" rbpc:C:/rb/pT.zip; rm -rf "$tmp"
RBVM=rbpc tools/win/vm.sh ps 'Expand-Archive -Force C:\rb\pT.zip C:\rb\pTx; robocopy C:\rb\pTx\platendoos C:\rb\platendoosT /E /NFL /NDL /NJH /NJS /NP | Out-Null; Remove-Item -Recurse -Force C:\rb\pTx, C:\rb\pT.zip; (ls C:\rb\platendoosT\rbmods.dll).LastWriteTime'
