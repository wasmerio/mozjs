#!/usr/bin/env bash

set -euo pipefail
set -x

working_dir="$(pwd)"
script_dir="$( cd "$( dirname "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
mode="${1:-release}"
mozconfig="${working_dir}/mozconfig-${mode}"
objdir="obj-$mode"

fetch_commits=
if [[ ! -a mozjs-wasi ]]; then

  # Clone Gecko repository at the required revision
  mkdir mozjs-wasi

  git -C mozjs-wasi init
  git -C mozjs-wasi remote add --no-tags -t wasi-embedding \
    origin "$(cat "$script_dir/wasi-mozjs-repository")"

  fetch_commits=1
fi

target_rev="$(cat "$script_dir/wasi-mozjs-revision")"
if [[ -n "$fetch_commits" ]] || \
  [[ "$(git -C mozjs-wasi rev-parse HEAD)" != "$target_rev" ]]; then
  git -C mozjs-wasi fetch --depth 1 origin "$target_rev"
  git -C mozjs-wasi checkout FETCH_HEAD
fi

cd mozjs-wasi

./mach --no-interactive bootstrap --application-choice=js --no-system-changes
