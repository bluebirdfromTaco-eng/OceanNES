#!/bin/bash
cd "$(dirname "$0")/.."
for p in $(seq 1 22); do echo $p; done | xargs -P 2 -I{} sh -c './actext ../AccuracyCoin/AccuracyCoin.nes {} 2600 > /tmp/claude-0/acpage_{}.txt'
tot=0; pass=0
for p in $(seq 1 22); do
  f=/tmp/claude-0/acpage_$p.txt
  pass=$((pass + $(grep -c "^ PASS" $f))); tot=$((tot + $(grep -cE "^ (PASS|FAIL|TEST)" $f)))
  grep -E "^ (FAIL|TEST)" $f | sed "s/^/p$p:/"
done
echo "AccuracyCoin: $pass / $tot passed"
