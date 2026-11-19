#!/usr/bin/env bash
# Builds tools/plugin/example_plugin.c into an importable mod: build/mods-import/example-plugin/plugin/ and its .zip
# next to it, the two shapes the launcher's Mods page imports (folder and archive). Linux .so only.
# Usage: tools/plugin/example.sh
set -euo pipefail
cd "$(dirname "$0")/../.."
root=build/mods-import
mod=$root/example-plugin
rm -rf "$mod" "$root/example-plugin.zip"
mkdir -p "$mod/plugin"
cc -std=c11 -O2 -Wall -Wextra -Werror -shared -fPIC -fvisibility=hidden -I runtime/plugin \
  tools/plugin/example_plugin.c -o "$mod/plugin/example_plugin.so"
(cd "$root" && zip -qr example-plugin.zip example-plugin)
echo "$mod"
echo "$root/example-plugin.zip"
