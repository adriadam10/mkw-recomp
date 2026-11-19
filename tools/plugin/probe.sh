#!/usr/bin/env bash
# The plugin API's test: builds tools/plugin/probe_plugin.c as a mod's plugin (build/mods/plugin-probe/plugin/) and
# runs build/mkw --probe-plugin with that mod on. Needs build/mkw and the game (game/, as every probe).
# Usage: tools/plugin/probe.sh
set -euo pipefail
cd "$(dirname "$0")/../.."
out=build/mods/plugin-probe/plugin
mkdir -p "$out"
cc -std=c11 -O2 -Wall -Wextra -Werror -shared -fPIC -fvisibility=hidden -I runtime/plugin \
  tools/plugin/probe_plugin.c -o "$out/probe_plugin.so"
MKW_MODS=plugin-probe build/mkw --probe-plugin
