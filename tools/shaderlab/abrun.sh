#!/bin/zsh
# abrun.sh <trace> <tag>: replay-profile a trace in gpudebug, export perf tables, fetch the main-pass outputs, end session.
# gpudebug sometimes reports a short "profile run" (~6 s) whose /performance tree stays empty: retry the profile.
# A replay of a 3x trace makes the GPUTools replay service grow to 22-28 GB RSS; on 2026-10-11 an unguarded replay
# (this script's guard path was broken) pushed a 64 GB Mac to 72.6 of 73.7 GB swap. Pre-flight refusals below.
set -u
# Heavy-tool mode (local agent docs): replays run only under tools/fable2_heavy.py (exclusive machine,
# preflight, external watchdog), after main confirms resources.
[[ -n "${FABLE2_HEAVY_RUN_ID:-}" ]] || { echo "refused: run via python3 tools/fable2_heavy.py run --kind replay -- zsh tools/shaderlab/abrun.sh <trace> <tag> (see the local agent docs)"; exit 2; }
TOOLS=${0:a:h}
cd ${SHADERLAB_DIR:-$TOOLS/../../out/shaderlab}
T=$1; TAG=$2; O=$PWD/ab/$TAG; mkdir -p $O
[[ -x $TOOLS/memwatch.sh ]] || { echo "refused: guard $TOOLS/memwatch.sh missing"; exit 2; }
free_mb=$(( $(memory_pressure -Q | sed -n 's/.*percentage: \([0-9]*\)%.*/\1/p') * $(sysctl -n hw.memsize) / 104857600 ))
swap_mb=$(sysctl -n vm.swapusage | sed -n 's/.*used = \([0-9]*\)\..*/\1/p')
min_mb=$(( ${SHADERLAB_MIN_FREE_GB:-40} * 1024 ))
if (( free_mb < min_mb || ${swap_mb:-0} > 4096 )); then
  echo "refused: free ${free_mb} MB (need ${min_mb}), swap used ${swap_mb} MB (max 4096)"; exit 2
fi
$TOOLS/memwatch.sh > $O/memwatch.log 2>&1 &
MW=$!
sleep 3
kill -0 $MW 2>/dev/null && grep -q avail_mb $O/memwatch.log || { echo "refused: guard did not start"; kill $MW 2>/dev/null; exit 2; }
out=$(printf 'status\n' | gpudebug -q -t $T --timeout 0 2>&1)
echo "$out" > $O/start.txt
S=$(echo "$out" | sed -n 's/^Session \([0-9]*\) created.*/\1/p' | head -1)
echo "session $S"
g() { gpudebug -s $S "$@"; }
ready() {
  g -c "go /performance/encoders" >/dev/null 2>&1
  g --json -c "list" 2>/dev/null | /usr/bin/python3 -c "import json,sys;c=json.load(sys.stdin).get('children',[]);print(len(c) if c and c[0]['name'][:2] in ('re','ce','be') else 0)" 2>/dev/null
}
ok=0
for attempt in 1 2 3; do
  g -c "wait" >/dev/null 2>&1
  g -c "profile run" >> $O/start.txt 2>&1
  for i in {1..12}; do n=$(ready); [[ -n "$n" && "$n" != "0" ]] && { ok=1; break; }; sleep 10; done
  [[ $ok == 1 ]] && break
  echo "attempt $attempt: empty profile, retrying" >> $O/start.txt
done
echo "profile ok=$ok" >> $O/start.txt
g --json -c "list --all" > $O/encoders.json
g -c "go /performance/shaders" >/dev/null; g --json -c "list --all" > $O/shaders.json
g -c "go /performance/encoders/re119/commands" >/dev/null; g --json -c "list --all" > $O/re119-cmds.json
g -c "go /performance/encoders" >/dev/null; g -c "info re119 --all" > $O/re119-info.txt
g -c "go /commands/cb7/ce16/grp0/disp0/compute" >/dev/null; g -c "fetch buf[1] --out $O/resolve_main.bin" >/dev/null
g -c "go /commands/cb7/re15" >/dev/null; g -c "fetch color0 --out $O/main_color0.png" >/dev/null
echo $S > $O/session.txt
[[ -n "${KEEP_SESSION:-}" ]] || gpudebug --terminate $S >/dev/null 2>&1
kill $MW
echo "$TAG ok=$ok peak: $(sort -t= -k2 -n $O/memwatch.log | grep replay | tail -1)"
