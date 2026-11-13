#!/bin/sh
#
# Copyright (c) 2026 Mark R V Murray.
#
# SPDX-License-Identifier: BSD-2-Clause
#
# Rebuild EVERYTHING from scratch and run every suite.
#
# Why from scratch, always. Incremental builds here have produced
# confidently wrong conclusions at least five times: a source is edited,
# make decides it has nothing to do, and the binary under test is the
# PREVIOUS one. Two shapes of it, both silent:
#
#   - restoring a backup with cp gives the file its ORIGINAL mtime,
#     older than the object, so nothing rebuilds;
#   - make's timestamps have one-second granularity, so an edit landing
#     in the same second as the last compile can leave the object stale
#     -- or rebuild the object and skip the LINK, which is worse,
#     because the object list looks freshly built.
#
# Twice this meant reading correct source while running sabotaged code
# and concluding the logic was wrong when the build was. It also cost the
# NitrOS-9 side a false bug report.
#
# The whole thing takes about thirty seconds (32.7 s for the six firmware
# trees in FW_TREES below, the parser tests and the usim suite, measured
# 2026-09-26 on this machine; it was fifteen with five trees, before
# build-rwdi-atfe and the larger usim suite). That buys
# out the entire class of fault, so there is no incremental path here on
# purpose: a script with a --fast option would get used on the day it
# mattered most.
#
# It also runs the HOST PARSER TESTS, which are a separate cmake tree the
# firmware build never touches and which therefore rot unnoticed -- one
# case sat red for weeks until an unrelated change happened to run them.
#
# Mark's cmake-build-* trees (including *-atfe-23) are deliberately NOT
# touched: they are his, and this script is the agent's.

set -u

# build-rwdi-atfe matters more than its position suggests: RelWithDebInfo +
# ATfE is the configuration Mark actually FLASHES, and it was missing from
# this list until 2026-09-26. Every other tree is Release, which is a
# different optimisation level, so a clean run here said nothing about the
# image that goes on the board.
FW_TREES="build build-host build-192 build-atfe build-noscan build-rwdi-atfe"
USIM="${USIM_DIR:-$HOME/GitHub/usim}"
JOBS="${JOBS:-8}"

fail=0
start=$(date +%s)

# iCloud CONFLICT COPIES. ~/Documents is iCloud-synced, so every rebuild's
# churn leaves "name 2", "name 12.d" and so on in the build trees -- 1,643 of
# them across these trees when first counted (2026-09-26), 822 in one. A clean
# rebuild does not remove them, since they are not names the build knows.
# Mark's standing rule is to delete them; this sweeps ONLY the trees this
# script owns. His cmake-build-* trees are not touched, and .git is handled
# separately (see the project memory), because a duplicate there can be a ref.
for t in $FW_TREES build-tests; do
	[ -d "$t" ] && find "$t" \( -name '* [0-9]' -o -name '* [0-9].*' \) \
	    -prune -exec rm -rf {} + 2>/dev/null
done

note() { printf '%-34s %s\n' "$1" "$2"; }

# --- firmware, every tree, every toolchain ---------------------------------
#
# Both toolchains matter and a clean gcc build says nothing about the
# clang image that actually gets flashed: the hot blitter's O3 attribute
# is gcc-only by construction, so the two compilers genuinely differ on
# the hot path.
# A TREE IS WHAT ITS NAME SAYS, OR THE RUN FAILS. The *atfe trees exist to
# build what Mark flashes, with ATfE's clang; every other tree is gcc. On
# 2026-09-27 both ATfE trees were reconfigured with gcc by accident (a zsh
# variable holding two -D options did not word-split, so PICO_COMPILER never
# reached CMake) and this script reported them green -- a clean gcc build,
# saying nothing about the image that goes on the board. So the compiler each
# tree was CONFIGURED with is checked against its name before anything builds.
tree_compiler_ok() {
	cc=$(sed -n 's/^CMAKE_C_COMPILER:[A-Z]*=//p' "$1/CMakeCache.txt" 2>/dev/null)
	case "$1" in
	*atfe*) case "$cc" in */ATfE*/clang) return 0 ;; esac ;;
	*)      case "$cc" in *arm-none-eabi-gcc) return 0 ;; esac ;;
	esac
	printf '%s is configured with "%s", which its name forbids\n' "$1" "$cc"
	return 1
}

for t in $FW_TREES; do
	# A missing tree FAILS the run. It used to be skipped, so with every
	# tree deleted (as after a toolchain upgrade, before reconfiguring)
	# the script built no firmware at all and still reported success.
	if [ ! -d "$t" ]; then
		note "$t" "ABSENT - not built"
		fail=1
		continue
	fi
	if ! tree_compiler_ok "$t"; then
		note "$t" "WRONG COMPILER - not built"
		fail=1
		continue
	fi
	if cmake --build "$t" -j"$JOBS" --clean-first >/tmp/rb.$$.log 2>&1; then
		note "$t" "built"
	else
		note "$t" "FAILED"
		sed -n '1,40p' /tmp/rb.$$.log
		fail=1
	fi
	rm -f /tmp/rb.$$.log
done

# --- host parser tests ----------------------------------------------------
if [ -d build-tests ]; then
	if cmake --build build-tests -j"$JOBS" --clean-first \
	    >/tmp/rb.$$.log 2>&1 &&
	   ctest --test-dir build-tests --output-on-failure \
	    >>/tmp/rb.$$.log 2>&1; then
		note "build-tests (parser)" "built + passed"
	else
		note "build-tests (parser)" "FAILED"
		tail -30 /tmp/rb.$$.log
		fail=1
	fi
	rm -f /tmp/rb.$$.log
else
	# Fails the run, as a missing firmware tree does: a suite that is not
	# there has not passed.
	note "build-tests (parser)" "ABSENT - cmake -S tests -B build-tests"
	fail=1
fi

# --- usim: the simulator's copy of the portable renderer ------------------
#
# Removing the objects is not enough on its own: the executable has to go
# too, or a same-second rebuild can relink nothing. That is the exact
# trap this script exists to remove, so it is spelled out rather than
# left to make.
if [ -d "$USIM" ]; then
	( cd "$USIM" &&
	  rm -f pv_*.o picovideo.o tests/test_picovideo &&
	  make -j"$JOBS" tests/test_picovideo >/tmp/rb.$$.log 2>&1 &&
	  ./tests/test_picovideo >>/tmp/rb.$$.log 2>&1 )
	if [ $? -eq 0 ]; then
		note "usim test_picovideo" "built + passed"
	else
		note "usim test_picovideo" "FAILED"
		tail -30 /tmp/rb.$$.log
		fail=1
	fi
	rm -f /tmp/rb.$$.log
else
	note "usim" "ABSENT at $USIM"
fi

printf '\n%s in %d s\n' \
    "$([ $fail -eq 0 ] && echo 'ALL GREEN' || echo 'SOMETHING FAILED')" \
    "$(( $(date +%s) - start ))"
exit $fail
