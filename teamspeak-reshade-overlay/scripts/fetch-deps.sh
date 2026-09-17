#!/usr/bin/env bash
# Fetches the third-party headers this project builds against. Nothing here is redistributed in
# the repository; each dependency is pinned to an exact tag so a build is reproducible.
#
# Usage: scripts/fetch-deps.sh [--reshade-tag vX.Y.Z] [--imgui-tag vX.Y.Z]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VENDOR="$ROOT/third_party"

# Pinned pair. Two separate constraints, both load-bearing:
#   1. ReShade resolves the ImGui function table by exact IMGUI_VERSION_NUM, so the two versions
#      must match the pairing in docs/compatibility.md.
#   2. It must be ImGui's *docking* branch. ReShade's imgui_function_table declares DockSpace,
#      ImGuiDockNodeFlags and ImGuiWindowClass, which exist only there; against master the
#      add-on does not compile.
RESHADE_TAG="v6.4.1"   # RESHADE_API_VERSION 16
IMGUI_TAG="v1.91.8-docking"  # IMGUI_VERSION_NUM 19180, docking branch (see below)
TS3_SDK_REF="master"   # the SDK is not tagged; pinned by commit below
TS3_SDK_COMMIT=""      # set to a commit hash to pin exactly

while [[ $# -gt 0 ]]; do
  case "$1" in
    --reshade-tag) RESHADE_TAG="$2"; shift 2 ;;
    --imgui-tag)   IMGUI_TAG="$2";   shift 2 ;;
    --ts3-commit)  TS3_SDK_COMMIT="$2"; shift 2 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

mkdir -p "$VENDOR"

fetch_sparse() {
  local url="$1" dir="$2" ref="$3" path="$4"
  if [[ -d "$dir/.git" ]]; then
    echo "==> $dir already present; skipping"
    return
  fi
  echo "==> fetching $url @ $ref"
  rm -rf "$dir"
  git clone --depth 1 --filter=blob:none --sparse --branch "$ref" "$url" "$dir"
  git -C "$dir" sparse-checkout set "$path"
}

fetch_sparse https://github.com/crosire/reshade.git "$VENDOR/reshade" "$RESHADE_TAG" include

if [[ ! -d "$VENDOR/imgui/.git" ]]; then
  echo "==> fetching Dear ImGui @ $IMGUI_TAG"
  rm -rf "$VENDOR/imgui"
  git clone --depth 1 --branch "$IMGUI_TAG" https://github.com/ocornut/imgui.git "$VENDOR/imgui"
else
  echo "==> $VENDOR/imgui already present; skipping"
fi

if [[ ! -d "$VENDOR/ts3client-pluginsdk/.git" ]]; then
  echo "==> fetching the TeamSpeak 3 Plugin SDK"
  rm -rf "$VENDOR/ts3client-pluginsdk"
  git clone --depth 1 --branch "$TS3_SDK_REF" \
    https://github.com/TeamSpeak-Systems/ts3client-pluginsdk.git \
    "$VENDOR/ts3client-pluginsdk"
  if [[ -n "$TS3_SDK_COMMIT" ]]; then
    git -C "$VENDOR/ts3client-pluginsdk" fetch --depth 1 origin "$TS3_SDK_COMMIT"
    git -C "$VENDOR/ts3client-pluginsdk" checkout "$TS3_SDK_COMMIT"
  fi
else
  echo "==> $VENDOR/ts3client-pluginsdk already present; skipping"
fi

echo
echo "Dependencies are in $VENDOR:"
echo "  ReShade SDK       $RESHADE_TAG"
echo "  Dear ImGui        $IMGUI_TAG"
echo "  TeamSpeak SDK     ${TS3_SDK_COMMIT:-$TS3_SDK_REF}"
echo
echo "Verify the ImGui pairing before building the add-on:"
echo "  grep -m1 'IMGUI_VERSION_NUM !=' $VENDOR/reshade/include/reshade_overlay.hpp"
echo "  grep -m1 'define IMGUI_VERSION_NUM' $VENDOR/imgui/imgui.h"
echo "These two numbers must be equal."
