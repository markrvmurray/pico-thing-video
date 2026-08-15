#!/usr/bin/env bash
#
# Merge the compile_commands.json from the two build roles into one union DB at
# the repo root, so clangd covers every source regardless of which USB role a
# file belongs to.
#
# The project has two mutually-exclusive build configs (see USB_HOST_MODE):
#   build/       — USB device role: has usb_descriptors.cpp, not usb_host.cpp
#   build-host/  — USB host role:   has usb_host.cpp, not usb_descriptors.cpp
# A single build's DB is therefore always missing one file. This produces a
# merged compile_commands.json = PRIMARY entries + any files only SECONDARY has.
# Shared files (usb.cpp, video.cpp, ...) use the PRIMARY context, so PRIMARY is
# the role you are actively editing. Default PRIMARY is the host build.
#
# Usage:  scripts/merge-compile-commands.sh [PRIMARY_DIR] [SECONDARY_DIR]
#   e.g.  scripts/merge-compile-commands.sh              # build-host wins
#         scripts/merge-compile-commands.sh build build-host   # device wins
#
# Re-run after (re)configuring either build — the merged file is a static
# snapshot, unlike the old build/ symlink. Requires jq.

set -euo pipefail

cd "$(dirname "$0")/.."          # repo root

PRIMARY="${1:-build-host}"
SECONDARY="${2:-build}"
OUT="compile_commands.json"

command -v jq >/dev/null || { echo "error: jq not found" >&2; exit 1; }

tmp="$(mktemp)"
trap 'rm -f "$tmp"' EXIT

p="$PRIMARY/compile_commands.json"
s="$SECONDARY/compile_commands.json"

if [[ -f "$p" && -f "$s" ]]; then
	# All PRIMARY entries, plus SECONDARY entries whose file PRIMARY lacks.
	jq -s '
		.[0] as $p | .[1] as $s |
		($p | map(.file)) as $pf |
		$p + ($s | map(.file as $f | select($pf | index($f) | not)))
	' "$p" "$s" > "$tmp"
	echo "merged: $PRIMARY (primary) + $SECONDARY -> $OUT ($(jq length "$tmp") entries)"
elif [[ -f "$p" ]]; then
	cp "$p" "$tmp"; echo "only $PRIMARY present -> $OUT"
elif [[ -f "$s" ]]; then
	cp "$s" "$tmp"; echo "only $SECONDARY present -> $OUT"
else
	echo "error: no compile_commands.json in $PRIMARY or $SECONDARY" >&2
	exit 1
fi

# Remove first: OUT may be the old symlink into build/, and writing through it
# would clobber that build's own DB.
rm -f "$OUT"
mv "$tmp" "$OUT"
trap - EXIT
