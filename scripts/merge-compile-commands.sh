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
# With SECONDARY omitted it is DISCOVERED by role rather than assumed by name:
# the script works out which role PRIMARY holds (by which of the two role-only
# sources its database mentions) and picks any other build tree supplying the
# opposite role.  That keeps the union correct for build trees this script has
# never heard of — IDE-generated ones included — instead of only the two
# conventionally named directories.
#
# CMake runs this after every firmware build (see the POST_BUILD hook), passing
# the tree being built as PRIMARY, so the merged database tracks whichever role
# was compiled last and cannot silently go stale.  Requires jq.

set -euo pipefail

cd "$(dirname "$0")/.."          # repo root

OUT="compile_commands.json"

command -v jq >/dev/null || { echo "error: jq not found" >&2; exit 1; }

# The two sources that exist in exactly one role each; a database naming one
# of them identifies its tree's role.
HOST_ONLY="usb_host.cpp"
DEVICE_ONLY="usb_descriptors.cpp"

db_has() { [ -f "$1/compile_commands.json" ] && grep -q "$2" "$1/compile_commands.json"; }

PRIMARY="${1:-}"
if [ -z "$PRIMARY" ]; then
	# No preference given: prefer a host tree, else whatever exists.
	for d in build-host build; do
		[ -f "$d/compile_commands.json" ] && PRIMARY="$d" && break
	done
	PRIMARY="${PRIMARY:-build-host}"
fi
PRIMARY="${PRIMARY%/}"

SECONDARY="${2:-}"
if [ -z "$SECONDARY" ]; then
	# Which role does PRIMARY lack?  Find any tree that supplies it.
	if db_has "$PRIMARY" "$HOST_ONLY"; then
		want="$DEVICE_ONLY"
	elif db_has "$PRIMARY" "$DEVICE_ONLY"; then
		want="$HOST_ONLY"
	else
		want=""          # PRIMARY has neither (or no database) — nothing to complete
	fi
	if [ -n "$want" ]; then
		for d in build-host build cmake-build-*; do
			[ "${d%/}" = "$PRIMARY" ] && continue
			if db_has "$d" "$want"; then
				SECONDARY="${d%/}"
				break
			fi
		done
	fi
fi

tmp="$(mktemp)"
trap 'rm -f "$tmp"' EXIT

p="$PRIMARY/compile_commands.json"
s="$SECONDARY/compile_commands.json"

if [[ -n "$SECONDARY" && -f "$p" && -f "$s" ]]; then
	# All PRIMARY entries, plus SECONDARY entries whose file PRIMARY lacks.
	jq -s '
		.[0] as $p | .[1] as $s |
		($p | map(.file)) as $pf |
		$p + ($s | map(.file as $f | select($pf | index($f) | not)))
	' "$p" "$s" > "$tmp"
	echo "merged: $PRIMARY (primary) + $SECONDARY -> $OUT ($(jq length "$tmp") entries)"
elif [[ -f "$p" ]]; then
	cp "$p" "$tmp"
	echo "no complementary role found -> $OUT ($PRIMARY only)"
else
	echo "error: no compile_commands.json in $PRIMARY" >&2
	exit 1
fi

# Remove first: OUT may be the old symlink into build/, and writing through it
# would clobber that build's own DB.
rm -f "$OUT"
mv "$tmp" "$OUT"
trap - EXIT
