#!/usr/bin/env bash

set -euo pipefail
set -x

current_branch="$(git rev-parse --abbrev-ref HEAD)"
target_branch="$current_branch-squashed"
git branch -D $target_branch || true
git checkout -b $target_branch
# Known commit hash of first ever commit
git reset --soft ede5ea4d5e7999babf6a26732972a7d3b2622d3e
git commit -m "Squash changes"
