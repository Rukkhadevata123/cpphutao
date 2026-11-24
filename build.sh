#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
toolchain_version=20261104
host_arch=$(uname -m)
case "$host_arch" in
    x86_64)  toolchain_sha256=919380403de87dca3c581c2053230cd298af5e891286628ba0613b7fed370926 ;;
    aarch64) toolchain_sha256=595e95bdffabfc62559fe26e30bdfbccf277cbaed4902793555015ee47306b94 ;;
    *) printf 'Unsupported Linux host architecture: %s\n' "$host_arch" >&2; exit 1 ;;
esac
toolchain_name="llvm-mingw-$toolchain_version-ucrt-ubuntu-22.04-$host_arch"
toolchain_url="https://github.com/mstorsjo/llvm-mingw/releases/download/$toolchain_version/$toolchain_name.tar.xz"
minhook_commit=c3fcafdc10146beb5919319d0683e44e3c30d537
minhook_url="https://codeload.github.com/TsudaKageyu/minhook/tar.gz/$minhook_commit"
minhook_sha256=31757833475be039c16359d4e276e718db6ba2ba61a6b870053a112106015822

for command in curl tar xz gzip sha256sum flock mktemp install; do
    if ! command -v "$command" >/dev/null; then
        printf 'Required command not found: %s\n' "$command" >&2
        exit 1
    fi
done

for file in gi-toolkit-ng.cpp hutao-minhook-ng.cpp config.ini LICENSE; do
    if [[ ! -f "$project_dir/$file" ]]; then
        printf 'Required project file is missing: %s\n' "$project_dir/$file" >&2
        exit 1
    fi
done

cache_dir="/tmp/hutao-minhook-ng-cpp-cache-$UID"
mkdir -p -m 700 -- "$cache_dir"
if [[ ! -O "$cache_dir" || -L "$cache_dir" ]]; then
    printf 'Expected a cache directory owned by the current user: %s\n' "$cache_dir" >&2
    exit 1
fi

work_dir=$(mktemp -d /tmp/hutao-minhook-ng-cpp.XXXXXXXX)
trap 'rm -rf -- "$work_dir"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
cd -- "$work_dir"
printf 'Build directory: %s\nDownload cache: %s\n' "$work_dir" "$cache_dir"

cache_archive() {
    local url=$1 archive=$2 digest=$3
    if [[ -f "$archive" ]] && printf '%s  %s\n' "$digest" "$archive" | sha256sum --check --status; then
        printf 'Reusing archive: %s\n' "$archive"
        return
    fi

    printf 'Downloading: %s\n' "$url"
    curl --fail --location --silent --show-error --retry 3 --output download.part "$url"
    printf '%s  download.part\n' "$digest" | sha256sum --check
    mv -fT -- download.part "$archive"
}

toolchain_dir="$cache_dir/$toolchain_name"
minhook_archive="$cache_dir/minhook-$minhook_commit.tar.gz"
# Concurrent builds share verified downloads, but each builds its own std module.
(
    flock -x 9
    if [[ ! -d "$toolchain_dir" ]]; then
        archive="$cache_dir/$toolchain_name.tar.xz"
        cache_archive "$toolchain_url" "$archive" "$toolchain_sha256"
        tar -xJf "$archive"
        mv -T -- "$toolchain_name" "$toolchain_dir"
    else
        printf 'Reusing toolchain: %s\n' "$toolchain_dir"
    fi
    cache_archive "$minhook_url" "$minhook_archive" "$minhook_sha256"
) 9>"$cache_dir/cache.lock"

tar -xzf "$minhook_archive"
minhook_dir="$work_dir/minhook-$minhook_commit"
win_cc="$toolchain_dir/bin/x86_64-w64-mingw32-clang"
win_cxx="$toolchain_dir/bin/x86_64-w64-mingw32-clang++"
"$win_cxx" --version

for unit in buffer hook trampoline hde/hde64; do
    "$win_cc" -Os -DNDEBUG -ffunction-sections -fdata-sections \
        -I"$minhook_dir/include" -I"$minhook_dir/src" -I"$minhook_dir/src/hde" \
        -c "$minhook_dir/src/$unit.c" -o "${unit##*/}.o"
done
"$toolchain_dir/bin/llvm-ar" rcs libMinHook.a buffer.o hook.o trampoline.o hde64.o

cpp_flags=(
    -std=c++23 -Os -DNDEBUG -fms-extensions
    -DUNICODE -D_UNICODE -DNOMINMAX -DWIN32_LEAN_AND_MEAN
    -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00
    -ffunction-sections -fdata-sections
)
"$win_cxx" "${cpp_flags[@]}" -Wno-reserved-module-identifier \
    -c "$toolchain_dir/share/libc++/v1/std.cppm" -fmodule-output=std.pcm -o std.o

target_flags=("${cpp_flags[@]}" -fmodule-file=std=std.pcm -static -Wl,--gc-sections,--no-insert-timestamp -s)
"$win_cxx" "${target_flags[@]}" -shared \
    "$project_dir/hutao-minhook-ng.cpp" std.o libMinHook.a -ladvapi32 -luser32 -o hutao-minhook-ng.dll
"$win_cxx" "${target_flags[@]}" -municode -mwindows \
    "$project_dir/gi-toolkit-ng.cpp" std.o -luser32 -o gi-toolkit-ng.exe

cp -- "$project_dir/config.ini" "$project_dir/LICENSE" .
cp -- "$minhook_dir/LICENSE.txt" MINHOOK-LICENSE.txt
artifacts_dir="$project_dir/artifacts"
mkdir -p -- "$artifacts_dir"
install -m 0755 gi-toolkit-ng.exe hutao-minhook-ng.dll "$artifacts_dir/"
install -m 0644 config.ini LICENSE MINHOOK-LICENSE.txt "$artifacts_dir/"
printf 'Release package ready: %s\n' "$artifacts_dir"
