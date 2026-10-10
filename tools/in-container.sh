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

release() {
  debug
  analyze
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
  shell) exec bash ;;
  *) echo "unknown command: ${1:-}" >&2; exit 2 ;;
esac
