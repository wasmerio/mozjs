script_dir="$( cd "$( dirname "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"

mkdir -p "${MOZ_OBJDIR}/mozjs-libs"

while read -r file; do
  cp ${MOZ_OBJDIR}/$file "${MOZ_OBJDIR}/mozjs-libs/"
done < "${script_dir}/object-files.list"

llvm-ar -r ${MOZ_OBJDIR}/mozjs-libs/libjs_static_extended.a ${MOZ_OBJDIR}/mozjs-libs/*.o
llvm-ranlib ${MOZ_OBJDIR}/mozjs-libs/libjs_static_extended.a
