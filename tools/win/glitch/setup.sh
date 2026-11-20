#!/bin/bash
# setup.sh <task>: (re)starts rekordbox (task platendoos or rbplain), closes the update prompt, loads Demo Track 1 on deck 1,
# plays it, opens the Spotify playlist Minimal Techno Mix; leaves a cropped screenshot in ~/rbwin/glitch/setup.png
here=$(cd "$(dirname "$0")" && pwd); cd "$here/../../.."
RBVM=rbpc tools/win/vm.sh stop
RBVM=rbpc tools/win/vm.sh ps "Start-ScheduledTask $1; Start-Sleep 30; '1128 622 left' | Set-Content -Encoding ascii C:\\rb\\click.txt; schtasks /run /tn rbclick | Out-Null; Start-Sleep 4; 'started'"
printf '0 wheel 16 931 5\n800 click 16 871\n2500 click 100 901\n4500 click 700 955\n5000 key shift+left\n8000 click 683 531\n9000 wheel 16 931 5\n9800 wheel 16 931 -2\n10800 click 16 859\n13000 click 165 959\n20000 click 1860 860\n24000 end\n' | GQ=1 "$here/g.sh" setup
RBVM=rbpc tools/win/vm.sh shot ~/rbwin/glitch/setup-full.png >/dev/null
python3 -c "
from PIL import Image
im=Image.open('$HOME/rbwin/glitch/setup-full.png'); o=Image.new('RGB',(1300,270)); o.paste(im.crop((0,830,1300,1010)),(0,0)); o.paste(im.crop((0,340,1300,420)),(0,184)); o.save('$HOME/rbwin/glitch/setup.png')"
