#!/bin/zsh
# Log replay-service RSS and available memory (memory_pressure free % of RAM) every 2 s.
# Guard: terminate this lab's gpudebug sessions if free memory < 8 GB, replay RSS > 24 GB or swap used > 8 GB.
while true; do
  tot=$(ps -axo rss=,comm= | egrep -i "gpudebug|GPUToolsDebugService|GPUToolsReplayService" | grep -v egrep | awk '{s+=$1} END{print int(s/1024)}')
  # Kernel's view (memory_pressure free %) x physical memory.
  avail=$(( $(memory_pressure -Q | sed -n 's/.*percentage: \([0-9]*\)%.*/\1/p') * $(sysctl -n hw.memsize) / 104857600 ))
  swap=$(sysctl -n vm.swapusage | sed -n 's/.*used = \([0-9]*\)\..*/\1/p')
  echo "$(date +%T) replay_rss_mb=$tot avail_mb=$avail swap_mb=$swap"
  if [[ ${avail:-0} -lt 8192 || ${tot:-0} -gt 24000 || ${swap:-0} -gt 8192 ]]; then
    echo "LIMIT: terminating gpudebug sessions"
    for s in $(gpudebug -l 2>/dev/null | awk -v re="${SHADERLAB_TRACE_RE:-^(cap3|o2|spec|spec2|stats[0-9]*)[.]gputrace$}" '$1 ~ /^[0-9]+$/ && $2 ~ re {print $1}'); do gpudebug --terminate $s; done
  fi
  sleep 2
done
