#!/usr/bin/env bash
# Runs inside the build image. Invoked via ./build on the host.
set -euo pipefail
cd /src

setup() {  # setup <dir> <meson options...>
  local dir=$1; shift
  if [[ -d $dir/meson-private ]]; then
    meson setup --reconfigure "$dir" -Dbuild_id="$GBK_VERSION" "$@" | tail -n 25
  else
    meson setup "$dir" -Dbuild_id="$GBK_VERSION" "$@" | tail -n 25
  fi
}

debug() {
  echo "== debug (ASan + UBSan) =="
  setup out/debug -Dbuildtype=debug -Db_sanitize=address,undefined
  meson compile -C out/debug
  meson test -C out/debug --print-errorlogs
}

analyze() {
  echo "== analyze (gcc -fanalyzer) =="
  setup out/analyze -Dbuildtype=debug -Danalyzer=true
  meson compile -C out/analyze
}

# fuzz [seconds-per-target]: libFuzzer (clang, ASan+UBSan) on every target. The working
# corpus persists in out/fuzz/corpus; crashing inputs land in out/fuzz/crashes.
# A crash fails the run: minimise it into tests/fuzz/seeds/<target>/ so it becomes a
# permanent regression test.
fuzz() {
  local secs=${1:-60}
  echo "== fuzz (${secs}s per target) =="
  CC=clang setup out/fuzz -Dbuildtype=debug -Dfuzzing=true \
    -Db_sanitize=address,undefined -Db_lundef=false
  meson compile -C out/fuzz
  mkdir -p out/fuzz/crashes
  local status=0
  for bin in out/fuzz/tests/fuzz-*; do
    [[ -f $bin && -x $bin ]] || continue
    local name=${bin##*/fuzz-}
    local corpus=out/fuzz/corpus/$name
    mkdir -p "$corpus"
    echo "-- $name"
    if ! "$bin" -max_total_time="$secs" -timeout=10 -max_len=4096 -print_final_stats=1 \
         -artifact_prefix="out/fuzz/crashes/$name-" "$corpus" "tests/fuzz/seeds/$name" \
         2>"out/fuzz/$name.log"; then
      echo "   CRASH: see out/fuzz/$name.log and out/fuzz/crashes/" >&2
      status=1
    fi
    grep -E "stat::number_of_executed_units|cov:" "out/fuzz/$name.log" | tail -2 | sed 's/^/   /'
  done
  return $status
}

release() {
  debug
  analyze
  fuzz 20
  echo "== release (hardened) =="
  # b_ndebug=false: assertions stay on in the shipped binary.
  setup out/release -Dbuildtype=debugoptimized -Db_ndebug=false -Dhardening=true
  meson compile -C out/release
  meson test -C out/release --print-errorlogs
  echo
  echo "built: out/release/src/gbk-file ($(out/release/src/gbk-file --version))"
}

case "${1:-}" in
  debug) debug ;;
  analyze) analyze ;;
  release) release ;;
  fuzz) fuzz "${2:-60}" ;;
  shell) exec bash ;;
  *) echo "unknown command: ${1:-}" >&2; exit 2 ;;
esac
