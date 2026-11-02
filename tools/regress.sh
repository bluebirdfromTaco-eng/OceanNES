#!/bin/bash
# Runs the automated (self-checking) test ROMs and prints a summary.
cd "$(dirname "$0")/.."
T=../testroms
ROMS="$T/instr_test-v5/rom_singles/*.nes $T/instr_misc/rom_singles/*.nes $T/instr_timing/rom_singles/*.nes
$T/cpu_dummy_writes/*.nes $T/cpu_exec_space/*.nes $T/cpu_interrupts_v2/rom_singles/*.nes $T/cpu_reset/*.nes
$T/branch_timing_tests/*.nes $T/ppu_vbl_nmi/rom_singles/*.nes $T/ppu_open_bus/*.nes $T/ppu_read_buffer/*.nes
$T/oam_read/*.nes $T/oam_stress/*.nes $T/sprite_hit_tests_2005.10.05/*.nes $T/sprite_overflow_tests/*.nes
$T/apu_test/rom_singles/*.nes $T/apu_reset/*.nes $T/mmc3_test_2/rom_singles/[1-5]*.nes $T/vbl_nmi_timing/*.nes
$T/blargg_apu_2005.07.30/*.nes $T/blargg_ppu_tests_2005.09.15b/*.nes"
pass=0; fail=0
run() { r=$1; out=$(timeout 60 ./runtest "$r" 2500 | tail -1);
  case "$out" in *PASS*) echo "ok   $(basename $r)";; *) if [[ "$r" == *blargg_* ]]; then
     ./runtest "$r" 900 /tmp/claude-0/x_$$.ppm >/dev/null; fi; echo "FAIL $(basename $r): $out";; esac; }
export -f run
for r in $ROMS; do echo $r; done | xargs -P 2 -I{} bash -c 'run {}' | sort > /tmp/claude-0/regress.txt
grep -c "^ok" /tmp/claude-0/regress.txt | sed 's/^/passed: /'; grep FAIL /tmp/claude-0/regress.txt
