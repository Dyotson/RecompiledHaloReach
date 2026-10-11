#!/usr/bin/env bash
# Build the ReXGlue Xenos GPU plugin (librexgpu-xenos.so) and the codegen tool
# (rexglue) from source with the fixes in patches/rexglue-sdk/, and install them
# into an overlay of the nightly SDK so the reach build (its codegen step and the
# plugin it ships) and tools/run_reach.sh pick them up.
#
# Usage: tools/build_rexglue_sdk.sh [--source-only]
#   --source-only  only check out and patch the SDK source (all tools/build_windows.sh needs)
#
# Why: the SDK's SPIR-V shader translator reads the texture-fetch exponent bias
# (exp_adjust) from fetch-constant word 4 instead of word 3. Reach's fetch
# constants carry a LOD bias in word 4, so every texture sample is scaled by
# 1/256 and the title menu renders black (docs/menu_black_screen.md). Upstream
# closed the report (rexglue/rexglue-sdk#456) without taking GPU changes, so we
# carry the patch.
#
# Environment:
#   REXSDK_NIGHTLY  installed nightly SDK the overlay is based on
#                   (default ~/rexglue-sdk-nightly/0.10.0.24/linux-amd64)
#   REXSDK_SRC      source checkout (default ~/rexglue-sdk-src/sdk)
#   REXSDK_PATCHED  overlay prefix to create
#                   (default ~/rexglue-sdk-patched/0.10.0.24/linux-amd64)
#
# Only the plugin and bin/rexglue are taken from this build; every other SDK file
# in the overlay is a symlink into the nightly, so both must be built from the
# nightly's exact commit to stay ABI-compatible with its librexruntime.so.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SDK_COMMIT=bd833a2ab69d305f99ddf57418fc38ffec8af93d  # nightly 0.10.0.24 (nightly-20261002-bd833a2a)
NIGHTLY="${REXSDK_NIGHTLY:-$HOME/rexglue-sdk-nightly/0.10.0.24/linux-amd64}"
SRC="${REXSDK_SRC:-$HOME/rexglue-sdk-src/sdk}"
OVERLAY="${REXSDK_PATCHED:-$HOME/rexglue-sdk-patched/0.10.0.24/linux-amd64}"
BREW=/home/linuxbrew/.linuxbrew

SOURCE_ONLY=0
[ "${1:-}" = --source-only ] && SOURCE_ONLY=1
[ "$SOURCE_ONLY" = 1 ] || [ -d "$NIGHTLY/lib" ] || { echo "nightly SDK not found at $NIGHTLY" >&2; exit 1; }

if [ ! -d "$SRC/.git" ]; then
    git clone https://github.com/rexglue/rexglue-sdk "$SRC"
fi
git -C "$SRC" fetch --quiet origin "$SDK_COMMIT" 2>/dev/null || true
git -C "$SRC" checkout --quiet "$SDK_COMMIT"
git -C "$SRC" submodule update --init --recursive --depth 1 --quiet

for patch in "$ROOT"/patches/rexglue-sdk/*.patch; do
    if git -C "$SRC" apply --reverse --check "$patch" 2>/dev/null; then
        echo "already applied: $(basename "$patch")"
    else
        git -C "$SRC" apply "$patch"
        echo "applied: $(basename "$patch")"
    fi
done
[ "$SOURCE_ONLY" = 0 ] || exit 0

# Building the SDK needs X11/XCB and Wayland headers (its UI module and SDL3
# require them). Hosts without them (immutable Fedora) can use Homebrew's:
#   brew install libx11 libxcb libxext libxfixes libxcursor libxi libxrandr \
#                libxscrnsaver libxrender libxtst xorgproto wayland
cmake_args=()
if [ -d "$BREW/opt/xorgproto/include" ]; then
    xp="$BREW/opt/xorgproto"
    inc=""
    for pkg in xorgproto wayland libxcb libx11 libxau libxdmcp; do
        inc+=" -I$BREW/opt/$pkg/include"
    done
    export PKG_CONFIG_PATH="$BREW/lib/pkgconfig:$BREW/share/pkgconfig:$xp/share/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
    # The -L only serves this build's own librexruntime.so, which the plugin
    # links against but which is never shipped (the nightly's is used).
    cmake_args+=(-DCMAKE_PREFIX_PATH="$BREW" -DX11_X11_INCLUDE_PATH="$BREW/include"
                 "-DCMAKE_C_FLAGS=-march=x86-64-v2 $inc"
                 "-DCMAKE_CXX_FLAGS=-march=x86-64-v2 $inc"
                 "-DCMAKE_SHARED_LINKER_FLAGS=-L$BREW/lib")
fi
( cd "$SRC" && cmake --preset linux-amd64 "${cmake_args[@]}" > /dev/null )
cmake --build "$SRC/out/build/linux-amd64" --config Release --target rexgpu-xenos rexglue rexruntime \
    --parallel "$(nproc)"

built="$SRC/out/linux-amd64/Release/librexgpu-xenos.so"
[ -n "$built" ] || { echo "librexgpu-xenos.so not found in the build tree" >&2; exit 1; }

# Overlay: symlink everything from the nightly, then replace the plugin.
mkdir -p "$OVERLAY/lib"
for entry in "$NIGHTLY"/*; do
    case "$(basename "$entry")" in
        lib|bin) ;;
        *) ln -sfn "$entry" "$OVERLAY/$(basename "$entry")" ;;
    esac
done
for entry in "$NIGHTLY"/lib/*; do
    ln -sfn "$entry" "$OVERLAY/lib/$(basename "$entry")"
done
rm -f "$OVERLAY/lib/librexgpu-xenos.so"
cp "$built" "$OVERLAY/lib/librexgpu-xenos.so"
echo "patched plugin: $OVERLAY/lib/librexgpu-xenos.so"

# The runtime (patches 0026, 0027: XAM/kernel fixes). Same commit as the nightly's, so it is
# a drop-in; give it the nightly's RUNPATH (its own is the build tree's).
runtime_new="$OVERLAY/lib/.librexruntime.new"
cp "$SRC/out/linux-amd64/Release/librexruntime.so" "$runtime_new"
rpath_script="$(mktemp)"
printf 'file(RPATH_SET FILE "%s" NEW_RPATH "$ORIGIN:$ORIGIN/../lib")\n' "$runtime_new" > "$rpath_script"
cmake -P "$rpath_script"
rm -f "$rpath_script"
rm -f "$OVERLAY/lib/librexruntime.so"
mv "$runtime_new" "$OVERLAY/lib/librexruntime.so"
echo "patched runtime: $OVERLAY/lib/librexruntime.so"

# The codegen tool: patches change the generated code too (0025: Xenon's dot-product
# overflow). Older overlays linked bin/ to the nightly's; it is a directory now.
[ -L "$OVERLAY/bin" ] && rm "$OVERLAY/bin"
mkdir -p "$OVERLAY/bin"
for entry in "$NIGHTLY"/bin/*; do
    [ "$(basename "$entry")" = rexglue ] || ln -sfn "$entry" "$OVERLAY/bin/$(basename "$entry")"
done
# The build tree's rexglue has that tree's absolute RUNPATH; give it the nightly's
# ($ORIGIN/../lib is the overlay's lib/, which links into the nightly).
tool_new="$OVERLAY/bin/.rexglue.new"
cp "$SRC/out/linux-amd64/Release/rexglue" "$tool_new"
rpath_script="$(mktemp)"
printf 'file(RPATH_SET FILE "%s" NEW_RPATH "$ORIGIN:$ORIGIN/../lib")\n' "$tool_new" > "$rpath_script"
cmake -P "$rpath_script"
rm -f "$rpath_script"
if cmp -s "$tool_new" "$OVERLAY/bin/rexglue"; then
    rm -f "$tool_new"
else
    rm -f "$OVERLAY/bin/rexglue"
    mv "$tool_new" "$OVERLAY/bin/rexglue"
    # Code generated by another tool is stale. The reach build reruns codegen when the
    # tool is newer than its output, but codegen skips modules whose stamp fingerprint
    # (its inputs, not the tool) matches: drop the stamps.
    rm -f "$ROOT"/reach-recomp/generated/*/codegen.stamp
fi
"$OVERLAY/bin/rexglue" --help > /dev/null
echo "patched codegen: $OVERLAY/bin/rexglue"

# Stage it next to existing builds of the same SDK right away (the reach
# POST_BUILD step only re-copies it when reach relinks). Builds against other
# SDK versions keep their own plugin: it must match their librexruntime.so.
for build in "$ROOT"/reach-recomp/out/build/*/; do
    [ -f "$build/librexgpu-xenos.so" ] || continue
    sdk_dir="$(sed -n 's/^rexglue_DIR:PATH=//p' "$build/CMakeCache.txt" 2>/dev/null)"
    case "$sdk_dir" in
        "$NIGHTLY"/*|"$OVERLAY"/*)
            # rm first: running instances may have the old file mapped.
            rm -f "$build/librexgpu-xenos.so"
            cp "$OVERLAY/lib/librexgpu-xenos.so" "$build/librexgpu-xenos.so"
            echo "staged into $build" ;;
    esac
done
