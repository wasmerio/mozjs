#!/usr/bin/env bash

set -euo pipefail
set -x

script_dir="$( cd "$( dirname "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"

fetch_commits=
if [[ ! -a mozjs ]]; then

    # Clone Gecko repository at the required revision
    mkdir mozjs

    git -C mozjs init
    git -C mozjs remote add --no-tags \
        origin "$(cat "$script_dir/gecko-repository")"

    fetch_commits=1
fi

target_rev="$(cat "$script_dir/gecko-revision")"
if [[ -n "$fetch_commits" ]] || \
    [[ "$(git -C mozjs rev-parse HEAD)" != "$target_rev" ]]; then
    git -C mozjs fetch --depth 1 origin "$target_rev"
    git -C mozjs checkout FETCH_HEAD
fi
