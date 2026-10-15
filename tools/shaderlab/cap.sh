#!/bin/zsh
# Capture run: 3x Bower Lake, one-frame gputrace at 16 s, labels, NS + ucode dumps, pass trace. Static camera until 30 s.
# Retired 2026-10-11: GPU captures run only under tools/fable2_heavy.py --kind gputrace, 1x, one frame, written
# to {out} (the local agent docs have the equivalent f2diag command). This 3x capture made 8 GB traces.
echo "refused: use python3 tools/fable2_heavy.py run --kind gputrace -- python3 tools/f2diag.py run sweep ... (see the local agent docs)"; exit 2
T=${0:a:h}; cd $T/../..
B=${SHADERLAB_DIR:-$PWD/out/shaderlab}; L=${1:-shaderlab-cap3}; export SHADERLAB_DIR=$B
mkdir -p $B/nsdump $B/ucode
/usr/bin/python3 $T/watchdog.py 16 &
/usr/bin/python3 tools/f2diag.py run sweep $L --save hero1-dog-live_2026-10-11_11-20-20 --fps 60 --sweep-plan spin5 --wait 30 --no-capture \
  --env FABLE2_RES_SCALE=${SCALE:-3} --env FABLE2_SHADER_LABELS=1 --env MTL_CAPTURE_ENABLED=1 \
  --env FABLE2_GPU_CAPTURE_PATH=$B/cap${SCALE:-3}.gputrace --env FABLE2_GPU_CAPTURE_AT_S=16 \
  --env FABLE2_NS_DUMP_DIR=$B/nsdump --env FABLE2_GPU_TIME_TRACE=1 \
  --game-arg=--dump_shaders=$B/ucode > $B/$L.log 2>&1
wait
pgrep -x fable_2; echo pgrep_rc=$?
