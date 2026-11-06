#!/bin/bash
# usage: run_matrix.sh <binary> <N>  — prints one classified line per run
BIN="$1"; N="$2"
crash=0; clean=0; other=0
for i in $(seq 1 "$N"); do
  LOG=$(mktemp)
  env -u LD_LIBRARY_PATH "$BIN" >"$LOG" 2>&1
  RC=$?
  tag="OTHER"
  if grep -qE 'munmap_chunk\(\): invalid pointer|free\(\): invalid pointer|corrupted' "$LOG"; then
    tag="CRASH-glibc-invalid-pointer"; 
  elif grep -qE 'SURVIVED|released cleanly' "$LOG"; then
    tag="clean-exit"
  fi
  if [ "$RC" = "134" ] || [ "$RC" = "139" ] || [ "$RC" = "1" ]; then :; fi
  LAST=$(grep -vE '^\s*$' "$LOG" | tail -1 | cut -c1-80)
  RB=$(grep -oE 'readback: [0-9]+/15 correct' "$LOG" | tail -1)
  echo "run $i: exit=$RC $tag ${RB:+[$RB] }last='$LAST'"
  case "$tag" in
    CRASH-glibc-invalid-pointer) crash=$((crash+1));;
    clean-exit) clean=$((clean+1));;
    *) other=$((other+1));;
  esac
  rm -f "$LOG"
done
echo "SUMMARY $BIN: crash=$crash clean=$clean other=$other of $N"
