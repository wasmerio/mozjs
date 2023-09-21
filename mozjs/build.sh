#! /usr/bin/env bash

set -ex

# WASI_SDK ?= /opt/wasi-sdk
export WASI_SDK=/Users/syrusakbary/Downloads/wasi-sdk-20.0

# export CC="$WASI_SDK/bin/clang --sysroot=$WASI_SDK/share/wasi-sysroot"
# export CXX="$WASI_SDK/bin/clang++ --sysroot=$WASI_SDK/share/wasi-sysroot"
# export CPP="$CXX"
# export CXXFLAGS="-fno-exceptions -DRUST_CXX_NO_EXCEPTIONS"
# export AR="$WASI_SDK/bin/ar"
# export LDFLAGS="-Wl,--shared-memory -Wl,--max-memory=4294967296 -Wl,--import-memory -Wl,--export-dynamic -Wl,--export=__heap_base -Wl,--export=__data_end -Wl,--export=__wasm_init_tls -Wl,--export=__wasm_signal -Wl,--export=__tls_size -Wl,--export=__tls_align -Wl,--export=__tls_base -lwasi-emulated-getpid"

# export LIBCLANG_PATH="$WASI_SDK/share/wasi-sysroot/lib/wasm32-wasi"
# export LIBCLANG_RT_PATH="$WASI_SDK/lib/clang/16/lib/wasi"

# mkdir lib

while read -r file; do
  cp "/Users/syrusakbary/Development/js-compute-runtime/runtime/spidermonkey/obj-release/$file" "lib"
done < "./object-files.list"

$AR -r libshit.a lib/*.o
llvm-ranlib libshit.a


if [ $# -eq 0 ]; then
  cargo build -vv --target=wasm32-wasi
else
  cargo "$@" --release --target=wasm32-wasi
fi
