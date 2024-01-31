#!/usr/bin/env bash

set -euo pipefail
set -x

working_dir="$(pwd)"
script_dir="$( cd "$( dirname "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
mode="${1:-release}"
mozconfig="${working_dir}/mozconfig-${mode}"
objdir="obj-$mode"

cd mozjs

./mach --no-interactive bootstrap --application-choice=js --no-system-changes
# ./mach configure --target=wasm32-wasi --with-wasi-sysroot=/Users/syrusakbary/Downloads/wasix-sysroot
