#! /usr/bin/env bash

set -ex

export WASI_SDK=/Users/syrusakbary/Downloads/wasi-sdk-20.0

if [ $# -eq 0 ]; then
  cargo build -vv --target=wasm32-wasi
else
  cargo "$@" --release --target=wasm32-wasi
fi
