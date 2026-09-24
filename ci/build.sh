#!/usr/bin/env bash
# ci/build.sh TARGET... - build, check and package the AutoBleem Store in the autobleem-build image
# (ghcr.io/autobleem2/autobleem-build). The CI workflow calls nothing else.
#
# The Store is built with the launcher (its AB_EXTENSION_DIRS names this folder - the launcher's
# docs/store-plan.md, until the SDK package exists), so this needs a launcher checkout: AB_LAUNCHER_DIR, by
# default ../autobleem (the workflow checks it out there, with its autobleem-core submodule). Only the Store's
# own targets are built - not the launcher's packages, not pcsx - into build/<target>/ here.
#
#   native   the tests (test_store_service, test_store_pictures, test_store_server) on the host, clang-format
#            --check over our sources, and abstored on its own for linux-x86_64 (server/CMakeLists.txt, the
#            launcher's autobleem-core as AB_CORE_DIR)
#   psc rpi rpi64 pcusb win
#            the extension for that platform -> dist/ext_store-<key>-<v>.zip, laid out as Extensions/store/
#            (extension.ini, lang/, bin/<key>/store.so|.dll) - what the Store's own site entry and a stick take
#   rpi rpi64 win pcusb also abstored for that machine -> dist/abstored-<os>-<arch>-<v>.tar.gz|.zip
#   all      every one of them
#
# The version is extension.ini's Version=, or AB_VERSION (the workflow's: the tag, or <Version>-<date>-<sha>).
# A plugin is never UPX-packed: a packed shared library does not load.
#
# On the build server:  docker run --rm -u $(id -u):$(id -g) -v $PWD/..:/w -w /w/ext_store \
#                           ghcr.io/autobleem2/autobleem-build:develop ci/build.sh all
set -euo pipefail
cd "$(dirname "$0")/.."
HERE="$PWD"
L="$(cd "${AB_LAUNCHER_DIR:-../autobleem}" 2>/dev/null && pwd)" || {
    echo "no launcher checkout at ${AB_LAUNCHER_DIR:-../autobleem} (AB_LAUNCHER_DIR)" >&2
    exit 1
}
[ -f "$L/autobleem-core/cmake/ab_extension.cmake" ] || {
    echo "$L has no autobleem-core submodule - clone it with --recurse-submodules" >&2
    exit 1
}
VERSION="${AB_VERSION:-$(sed -n 's/^Version=//p' extension.ini | tr -d '\r')}"
JOBS="${JOBS:-$(nproc)}"
LAUNCH=()
if [ -z "${AB_NO_SCCACHE:-}" ] && command -v sccache >/dev/null 2>&1; then
    LAUNCH=(-DCMAKE_C_COMPILER_LAUNCHER=sccache -DCMAKE_CXX_COMPILER_LAUNCHER=sccache)
fi

banner() { printf '\n==== %s ====\n' "$*"; }
configure() { # configure <dir> <cmake args...> - the launcher's tree with this folder as its one extension
    local dir="$1"
    shift
    cmake -S "$L" -B "$dir" -G Ninja "${LAUNCH[@]}" -DAB_EXTENSION_DIRS="$HERE" "$@" >/dev/null
}
zipdir() { # zipdir <dir> <zip> - every file under <dir>, paths relative to it, with their modes
    python3 - "$1" "$2" <<'EOF'
import os, sys, zipfile
root, out = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    for base, dirs, files in os.walk(root):
        dirs.sort()
        for name in sorted(files):
            path = os.path.join(base, name)
            z.write(path, os.path.relpath(path, root))
EOF
}
package_extension() { # package_extension <build dir> <key> - dist/ext_store-<key>-<v>.zip
    local dir="$1" key="$2" plugin
    local stage="$dir/extensions/store"
    plugin="$(ls "$stage/bin/$key"/store.* 2>/dev/null | head -1)"
    [ -n "$plugin" ] || { echo "no plugin under $stage/bin/$key" >&2; exit 1; }
    local pkg="$dir/package"
    rm -rf "$pkg"
    mkdir -p "$pkg/Extensions"
    cp -a "$stage" "$pkg/Extensions/store"
    sed -i "s/^Version=.*/Version=$VERSION/" "$pkg/Extensions/store/extension.ini"
    cp LICENSE "$pkg/Extensions/store/"
    mkdir -p dist
    rm -f "dist/ext_store-$key-$VERSION.zip"
    zipdir "$pkg" "dist/ext_store-$key-$VERSION.zip"
    ls -l "dist/ext_store-$key-$VERSION.zip"
}
package_server() { # package_server <abstored binary> <os-arch> - dist/abstored-<os-arch>-<v>.tar.gz|.zip
    local bin="$1" name="$2" extra="${3:-}"
    local pkg="build/abstored-$name"
    rm -rf "$pkg"
    mkdir -p "$pkg/abstored" dist
    cp "$bin" "$pkg/abstored/"
    [ -n "$extra" ] && cp "$extra" "$pkg/abstored/"
    cp server/README.md LICENSE "$pkg/abstored/"
    if [[ "$bin" == *.exe ]]; then
        rm -f "dist/abstored-$name-$VERSION.zip"
        zipdir "$pkg" "dist/abstored-$name-$VERSION.zip"
        ls -l "dist/abstored-$name-$VERSION.zip"
    else
        tar -czf "dist/abstored-$name-$VERSION.tar.gz" -C "$pkg" abstored
        ls -l "dist/abstored-$name-$VERSION.tar.gz"
    fi
}

# --- native: the gate --------------------------------------------------------------------------------------
build_native() {
    banner "native: the Store's tests (build/native)"
    configure build/native -DCMAKE_BUILD_TYPE=Debug -DAB_ENABLE_CHD=ON
    ninja -C build/native -j "$JOBS" store test_store_service test_store_pictures test_store_server
    ctest --test-dir build/native -R '^test_store_' --output-on-failure -j "$JOBS"

    banner "native: clang-format"
    local cf
    cf="$(command -v clang-format-22 || command -v clang-format)"
    find src server/src tests -name '*.cpp' -o -name '*.h' | sort | xargs "$cf" --dry-run --Werror

    banner "native: abstored on its own, linux-x86_64 (build/abstored)"
    cmake -S server -B build/abstored -G Ninja "${LAUNCH[@]}" -DCMAKE_BUILD_TYPE=Release \
        -DAB_CORE_DIR="$L/autobleem-core" >/dev/null
    ninja -C build/abstored -j "$JOBS" abstored
    strip build/abstored/abstored
    build/abstored/abstored --version
    package_server build/abstored/abstored linux-x86_64
}

# --- the targets ---------------------------------------------------------------------------------------------
build_psc() {
    local toolchain="${AB_PSC_TOOLCHAIN:-/opt/psc}"
    banner "psc: the console (build/psc, toolchain $toolchain)"
    configure build/psc -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE="$L/toolchains/psc/PSCtoolchainV8.cmake" -DAB_PSC_TOOLCHAIN="$toolchain"
    ninja -C build/psc -j "$JOBS" store
    # the console's glibc 2.24 / GLIBCXX 3.4.22, no RPATH; and a plugin that binds to the launcher's SDK
    bash "$L/tools/check_psc_binary.sh" build/psc/extensions/store/bin/psc/store.so "$toolchain"
    bash tools/check_extension.sh build/psc/extensions/store/bin/psc/store.so "$toolchain"
    package_extension build/psc psc
}

build_rpi() { # build_rpi rpi|rpi64
    local key="$1" dir="build/$1" toolchain proc arch
    case "$key" in
        rpi) toolchain=toolchains/rpi/RPitoolchain.cmake proc=arm arch=armhf ;;
        rpi64) toolchain=toolchains/rpi64/RPi64toolchain.cmake proc=aarch64 arch=arm64 ;;
    esac
    banner "$key: Raspberry Pi ($dir)"
    configure "$dir" -DCMAKE_SYSTEM_PROCESSOR="$proc" -DCMAKE_BUILD_TYPE=Release -DAB_RPI_DEBUG=OFF \
        -DCMAKE_TOOLCHAIN_FILE="$L/$toolchain"
    ninja -C "$dir" -j "$JOBS" store abstored
    file "$dir/extensions/store/bin/$key/store.so"
    case "$arch" in
        armhf) file "$dir/extensions/store/bin/$key/store.so" | grep -q 'ELF 32-bit LSB.*ARM, EABI5' ;;
        arm64) file "$dir/extensions/store/bin/$key/store.so" | grep -q 'ELF 64-bit LSB.*ARM aarch64' ;;
    esac
    bash tools/check_extension.sh "$dir/extensions/store/bin/$key/store.so"
    package_extension "$dir" "$key"
    case "$arch" in
        armhf) arm-linux-gnueabihf-strip "$dir/extensions/store-server/abstored" ;;
        arm64) aarch64-linux-gnu-strip "$dir/extensions/store-server/abstored" ;;
    esac
    package_server "$dir/extensions/store-server/abstored" "linux-$arch"
}

build_pcusb() {
    banner "pcusb: the 32-bit PC stick (build/pcusb)"
    configure build/pcusb -DCMAKE_BUILD_TYPE=Release -DAB_PCUSB_DEBUG=OFF -DAB_ENABLE_CHD=ON \
        -DCMAKE_TOOLCHAIN_FILE="$L/toolchains/pcusb/PcUsbToolchain.cmake"
    ninja -C build/pcusb -j "$JOBS" store abstored
    file build/pcusb/extensions/store/bin/pcusb/store.so | grep -q 'ELF 32-bit LSB.*Intel 80386'
    bash tools/check_extension.sh build/pcusb/extensions/store/bin/pcusb/store.so
    package_extension build/pcusb pcusb
    strip build/pcusb/extensions/store-server/abstored
    package_server build/pcusb/extensions/store-server/abstored linux-i386
}

build_win() {
    # the Windows product (AB_TARGET=win): its plugin imports from autobleem-gui.exe by name, so the launcher
    # is built here too - the import library comes from it
    banner "win: the Windows product (build/win)"
    configure build/win -DCMAKE_BUILD_TYPE=Release -DAB_ENABLE_CHD=ON -DAB_TARGET=win \
        -DCMAKE_TOOLCHAIN_FILE="$L/toolchains/mingw/MinGWtoolchain.cmake"
    ninja -C build/win -j "$JOBS" store abstored
    local dll=build/win/extensions/store/bin/win/store.dll
    file "$dll" | grep -q 'PE32+ executable.*DLL.*x86-64'
    # it binds to the launcher, not to a copy of the SDK of its own
    objdump -p "$dll" | grep 'DLL Name: autobleem-gui.exe' >/dev/null ||
        { echo "$dll does not import from autobleem-gui.exe" >&2; exit 1; }
    package_extension build/win win
    local exe=build/win/extensions/store-server/abstored.exe
    x86_64-w64-mingw32-strip "$exe"
    # winpthread: the one GCC runtime DLL the exe still needs (libgcc/libstdc++ are linked in)
    local pthread=""
    if objdump -p "$exe" | grep -q 'DLL Name: libwinpthread-1.dll'; then
        for candidate in /usr/x86_64-w64-mingw32/lib/libwinpthread-1.dll \
            /usr/lib/gcc/x86_64-w64-mingw32/*-posix/libwinpthread-1.dll; do
            [ -f "$candidate" ] && { pthread="$candidate"; break; }
        done
        [ -n "$pthread" ] || { echo "libwinpthread-1.dll not found" >&2; exit 1; }
    fi
    package_server "$exe" windows-x86_64 "$pthread"
}

# --- main ----------------------------------------------------------------------------------------------------
[ $# -gt 0 ] || { sed -n '2,22p' "$0"; exit 2; }
echo "ext_store $VERSION against the launcher at $L ($(git -C "$L" describe --tags --always 2>/dev/null || echo '?'))"
for target in "$@"; do
    case "$target" in
        native) build_native ;;
        psc) build_psc ;;
        rpi | rpi64) build_rpi "$target" ;;
        pcusb) build_pcusb ;;
        win) build_win ;;
        all) build_native; build_psc; build_rpi rpi; build_rpi rpi64; build_pcusb; build_win ;;
        *) echo "unknown target: $target (native, psc, rpi, rpi64, pcusb, win, all)" >&2; exit 2 ;;
    esac
done
[ -d dist ] && { banner "dist"; ls -l dist; }
