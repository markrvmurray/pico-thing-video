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
# The whole thing takes about fifteen seconds (13.6 s for five firmware
# trees, 1 s for usim, measured 2026-09-26 on this machine). That buys
# out the entire class of fault, so there is no incremental path here on
# purpose: a script with a --fast option would get used on the day it
# mattered most.
#
# It also runs the HOST PARSER TESTS, which are a separate cmake tree the
# firmware build never touches and which therefore rot unnoticed -- one
# case sat red for weeks until an unrelated change happened to run them.
#
# Mark's cmake-build-* trees (including *-atfe-22) are deliberately NOT
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

note() { printf '%-34s %s\n' "$1" "$2"; }

# --- firmware, every tree, every toolchain ---------------------------------
#
# Both toolchains matter and a clean gcc build says nothing about the
# clang image that actually gets flashed: the hot blitter's O3 attribute
# is gcc-only by construction, so the two compilers genuinely differ on
# the hot path.
for t in $FW_TREES; do
	if [ ! -d "$t" ]; then
		note "$t" "ABSENT - skipped"
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
	note "build-tests (parser)" "ABSENT - configure it"
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
