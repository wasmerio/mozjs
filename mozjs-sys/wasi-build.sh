#!/usr/bin/env bash

set -euo pipefail
set -x

working_dir="$(pwd)"
script_dir="$( cd "$( dirname "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
mode="${1:-release}"
objdir="obj-$mode"
# MOZ_OBJDIR="${script_dir}/${objdir}"
# TARGET="wasm32-wasmer-wasi"

mozconfig="${script_dir}/mozconfig-${mode}"

cat << EOF > "$mozconfig"
mk_add_options AUTOCLOBBER=1
mk_add_options MOZ_OBJDIR=${MOZ_OBJDIR}
ac_add_options --enable-project=js
ac_add_options --enable-application=js
ac_add_options --enable-jitspew
ac_add_options --enable-optimize=-O3
ac_add_options --enable-js-streams
ac_add_options --without-system-zlib
ac_add_options --without-intl-api
ac_add_options --disable-tests
ac_add_options --disable-clang-plugin
ac_add_options --disable-jit
ac_add_options --target=${TARGET}
ac_add_options --disable-js-shell
ac_add_options --disable-export-js
ac_add_options --disable-shared-js
ac_add_options --build-backends=RecursiveMake
EOF


target="$(uname)"
case "$target" in
  Linux)
    echo "ac_add_options --disable-stdcxx-compat" >> "$mozconfig"
    ;;

  Darwin)
    echo "ac_add_options --host=aarch64-apple-darwin" >> "$mozconfig"
    ;;

  *)
    echo "Unsupported build target: $target"
    exit 1
    ;;
esac

case "$mode" in
  release)
    echo "ac_add_options --disable-debug" >> "$mozconfig"
    ;;

  debug)
    echo "ac_add_options --enable-debug" >> "$mozconfig"
    ;;

  *)
    echo "Unknown build type: $mode"
    exit 1
    ;;
esac

cd mozjs-wasi

MOZCONFIG="${mozconfig}" \
  ./mach build

mkdir -p "${MOZ_OBJDIR}/mozjs-libs"

while read -r file; do
  cp "${MOZ_OBJDIR}/$file" "${MOZ_OBJDIR}/mozjs-libs/"
done < "${script_dir}/wasi-object-files.list"

llvm-ar -r ${MOZ_OBJDIR}/mozjs-libs/libjs_static_extended.a ${MOZ_OBJDIR}/mozjs-libs/*.o
llvm-ranlib ${MOZ_OBJDIR}/mozjs-libs/libjs_static_extended.a
