#!/bin/bash
# usage: acpage.sh page frames out.png
p=$1; fr=$2; out=$3; s=""; f=60
for ((i=1;i<p;i++)); do s="$s$f:80:3,"; f=$((f+10)); done
s="$s$((f+10)):01:3"
./runtest ../AccuracyCoin/AccuracyCoin.nes $fr /tmp/claude-0/acp_$p.ppm "$s" >/dev/null
python3 -c "
from PIL import Image; Image.open('/tmp/claude-0/acp_$p.ppm').save('$out')"
