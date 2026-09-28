cmake_minimum_required(VERSION 3.14)  # 3.14+ for BISON_TARGET DEFINES_FILE
# Roll-out board: the v2.0 PCB carries a Pimoroni PGA2350 (+ TPS2051B USB-C
# front end). Swap back to the SolderParty carrier for dev-board work.
set(PICO_BOARD pimoroni_pga2350)
#set(PICO_BOARD solderparty_rp2350_stamp_xl_carrier)

# Locate the Pico SDK import script.  PICO_SDK_PATH may arrive either as an
# environment variable (command-line builds that `export` it) or as a CMake
# cache variable via -DPICO_SDK_PATH=... (CLion / IDE builds pass it this way).
# Accept both — relying on $ENV{} alone silently breaks IDE builds: the SDK
# toolchain never loads, project() configures as a native macOS build, and the
# bare-metal cross-compiler is handed an -arch arm64 flag it cannot understand.
if(NOT PICO_SDK_PATH AND DEFINED ENV{PICO_SDK_PATH})
    set(PICO_SDK_PATH $ENV{PICO_SDK_PATH})
endif()
if(NOT PICO_SDK_PATH)
    message(FATAL_ERROR
        "PICO_SDK_PATH is not set. Provide it as an environment variable or with -DPICO_SDK_PATH=<path>.")
endif()
include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)

# USB role, selected at build time (the board carries a single native USB PHY,
# so it is host OR device, never both). Default OFF = USB-CDC device (today's
# behaviour). ON = USB HID host (keyboard + mouse via a hub), and the USB-CDC
# console is unavailable — the console stays on UART0. This one switch drives
# both the linked TinyUSB target and the -DUSB_HOST_MODE compile flag that
# tusb_config.h and the sources key off, so the two can never drift apart.
# Default ON since the hardware-bring-up era (2026-09): the keyboard is the
# point of the port, and the default must survive CLion regenerating its
# build trees' caches from profile options. The device/CDC role remains fully
# supported — configure with -DUSB_HOST_MODE=OFF (the build/ regression tree
# pins it OFF).
option(USB_HOST_MODE "Build USB as HID host (keyboard+mouse) instead of CDC device" ON)

# Run clk_sys at 320 MHz, with clk_hstx decoupled onto PLL_USB so the DVI pixel
# clock stays at 38.4 MHz, and the core voltage raised to 1.30 V. A BUILD OPTION
# rather than a source #define so the clock is a configuration rather than an
# edit: both can coexist in separate trees, and an .elf cannot silently become
# the other on a later rebuild.
#
# DEFAULT ON since 2026-09-23, on measurement rather than preference. Against
# 192 MHz on the same source: the bus read handler's worst case falls from
# 277 cycles (1.44 us) to 114 (0.35 us), and the worst render burst from 28.1 ms
# to 9.2 ms. The safety argument is the deciding one -- at 192 MHz that worst
# case is 144% of a 1 MHz 6809 bus cycle, longer than the interval in which a
# second access can arrive, which is exactly when pop-on-read serves a stale
# byte; at 320 MHz it is 35%.
#
# Costs: 1.30 V core (up from 1.10) and roughly 4-7 C on the die depending on
# load. Configure with -DFAST_SYS_CLK_320=OFF for a 192 MHz image; build-192/
# is kept as that comparison tree.
#
# NOTE, as for USB_HOST_MODE: changing this default does NOT override an
# existing cache entry. After flipping it, purge stale values with
# "cmake -U FAST_SYS_CLK_320 -B <tree>" or the tree silently keeps the old
# clock -- and the only outward sign is a line in the boot banner.
option(FAST_SYS_CLK_320 "Run clk_sys at 320 MHz with clk_hstx on PLL_USB" ON)

# MEASUREMENT BUILD ONLY -- leaves the display dark. The scan-out DMA reads the
# framebuffer continuously at a fixed wall rate and outranks both cores on the
# SRAM crossbar (see bus_ctrl_hw->priority in video.cpp), which is the leading
# explanation for the fixed-time stall inside the bus ISRs: the expensive read
# path measures 77 cycles of work plus ~172 ns that does not shrink with the
# clock. Turning scan-out off removes that traffic, so if the stall is the DMA
# it should collapse and the handler should cost ~77 cycles at ANY clock. That
# turns an inference into a measurement, and it decides whether the fix is to
# trim the handler's SRAM traffic or to move pop-on-read into the PIO.
option(VIDEO_SCANOUT "Run the scan-out DMA (OFF = dark display, measurement only)" ON)

# On-hardware self-tests (console "selftest"). Default ON, including Release:
# they are flash-resident, cost no SRAM (they scratch in PSRAM and the
# framebuffer, which matters -- the device-role build has ~648 bytes of SRAM
# left), and run only when an operator asks. Debug-only was considered and
# rejected: the flashed images are RelWithDebInfo/ATfE, so gating on a build
# type risks the tests existing only in binaries nobody runs. OFF drops them.
option(SELFTEST "Compile in the on-hardware self-tests" ON)

# PSRAM size is DETECTED, not taken from the board header, and the reason is
# not fussiness. In the header-only path the SDK never calls
# gpio_set_function(PICO_PSRAM_CS_PIN, GPIO_FUNC_XIP_CS1) at all -- that call
# lives inside its "#if PICO_AUTO_DETECT_PSRAM_SIZE" block -- so GPIO 47 never
# drives chip select, every access goes nowhere, and psram_get_size() still
# cheerfully reports the 8 MB the header claimed. Observed on hardware
# 2026-09-24: configured, reported, and completely inert.
#
# Auto-detect fixes both halves. It assigns the pin its XIP_CS1 function, and
# it MEASURES the size instead of believing the header -- and the SDK is
# explicit that only the auto-detect paths verify a part is present at all.
# The CS pin still comes from the board header, so nothing wiggles pins
# hunting for it.

# Flex and bison run on the BUILD HOST; the generated .c files are
# cross-compiled for RP2350 by arm-none-eabi-gcc.
#
# macOS ships bison 2.3 which is too old.  Use Homebrew's bison 3.x.
# Install with: brew install bison
if(APPLE)
    find_program(_bison_homebrew
        NAMES bison
        PATHS /opt/homebrew/opt/bison/bin /usr/local/opt/bison/bin
        NO_DEFAULT_PATH
    )
    if(NOT _bison_homebrew)
        message(FATAL_ERROR
            "Bison 3.x not found.  Install with: brew install bison")
    endif()
    # Force-set so CLion's stale cache (pointing at /usr/bin/bison 2.3) is overridden.
    set(BISON_EXECUTABLE "${_bison_homebrew}" CACHE FILEPATH "Bison executable" FORCE)
endif()
find_package(FLEX  REQUIRED)
find_package(BISON 3.0 REQUIRED)

project(pico-thing-video C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Keep build trees out of Time Machine: they churn on every build and can be
# regenerated.  A sticky exclusion (no -p) is stored on the directory itself,
# so unlike a fixed-path one it needs no root; it goes if the directory is
# deleted and comes back at the next configure.  CMAKE_HOST_APPLE, not APPLE:
# this project cross-compiles for the RP2350, so APPLE is false here.
if(CMAKE_HOST_APPLE)
    find_program(_tmutil tmutil NO_CMAKE_FIND_ROOT_PATH)
    if(_tmutil)
        execute_process(COMMAND ${_tmutil} addexclusion "${CMAKE_BINARY_DIR}"
            RESULT_VARIABLE _tmutil_result OUTPUT_QUIET ERROR_QUIET)
        if(NOT _tmutil_result EQUAL 0)
            message(STATUS "Could not exclude ${CMAKE_BINARY_DIR} from Time Machine")
        endif()
    endif()
endif()

# Emit build/compile_commands.json on every configure so clangd/CLion stay
# in sync with the real cross-build flags.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Trick target
add_custom_target(clion_host_dummy ALL)

add_compile_definitions(DEBUG)

pico_sdk_init()

# ----- Flex / Bison targets -----
# flex/bison run on the BUILD HOST; the generated .c files are
# cross-compiled for RP2350 by arm-none-eabi-gcc (newlib has all needed symbols).
# DEFINES_FILE pins the header name to command_parser.tab.h to avoid a
# circular-include collision with our project header command_parser.h.
FLEX_TARGET(CmdLexer
	${CMAKE_CURRENT_LIST_DIR}/src/command_lexer.l
	${CMAKE_CURRENT_BINARY_DIR}/command_lexer.c
)

BISON_TARGET(CmdParser
	${CMAKE_CURRENT_LIST_DIR}/src/command_parser.y
	${CMAKE_CURRENT_BINARY_DIR}/command_parser.c
	DEFINES_FILE ${CMAKE_CURRENT_BINARY_DIR}/command_parser.tab.h
)

ADD_FLEX_BISON_DEPENDENCY(CmdLexer CmdParser)

add_executable(pico-thing-video
		src/bus_engine.cpp
		src/console.cpp
		src/pico_thing_video.cpp
		src/power.cpp
		src/sound.cpp
		src/i2c_service.cpp
		src/registers.cpp
		src/usb.cpp
		src/vid_console.cpp
		src/gfx_bufs.cpp
		src/gfx_shadow.cpp
		src/win_store.cpp
		src/vid_link.cpp
		src/video.cpp
		src/cmd_trace.cpp
		src/fill_log.cpp
		src/mem_ops.c
		src/psram_bench.cpp
		src/selftest.cpp
		src/video_render.cpp
)

# USB role-specific sources: the device build needs its descriptors; the host
# build needs the HID/hub callbacks + input cooking instead (and never uses the
# device descriptors).
if(USB_HOST_MODE)
	target_sources(pico-thing-video PRIVATE src/usb_host.cpp src/hid_cook.cpp)
else()
	target_sources(pico-thing-video PRIVATE src/usb_descriptors.cpp)
endif()

target_compile_definitions(pico-thing-video PRIVATE PICO_AUTO_DETECT_PSRAM_SIZE=1)

if(SELFTEST)
	target_compile_definitions(pico-thing-video PRIVATE SELFTEST=1)
endif()

# Our own memcpy/memmove/memset/bzero, because every C library ATfE ships is a
# SIZE build whose versions move one byte per iteration (see mem_ops.c). These
# flags are not optional: without them a compiler may recognise a copy loop as
# "a memcpy" and replace it with a call to memcpy -- from inside memcpy.
set(_mem_ops_flags -ffreestanding -fno-builtin)
if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
	list(APPEND _mem_ops_flags -fno-tree-loop-distribute-patterns)
endif()
set_source_files_properties(src/mem_ops.c PROPERTIES
	COMPILE_OPTIONS "${_mem_ops_flags}")

# The renderer at -O3 in every optimised build, whatever the build type and
# whichever compiler. It used to be a GCC-only function attribute on the text
# blitter, empty under clang -- so the image actually flashed (RelWithDebInfo,
# ATfE) ran the hot path at -O2 while the source claimed -O3 (VID-030). A
# per-source option works for both compilers and cannot silently become a
# no-op. Appended after the build type's own flags, so it is the last -O on
# the line and wins. NOT in Debug builds: -O3 would make the renderer
# impossible to step through, which is the one thing a Debug build is for.
set_property(SOURCE src/video_render.cpp APPEND PROPERTY
	COMPILE_OPTIONS "$<$<NOT:$<CONFIG:Debug>>:-O3>")

# The BUILD IDENTITY, compiled in so the running board can say what it is.
# This exists because it was got wrong in conversation: five agent trees were
# Release while the image actually being flashed was RelWithDebInfo/ATfE, and
# nothing in "status" could have settled it. A figure whose meaning depends on
# the optimisation level -- which is most of them on that page -- is not
# comparable between builds, so the page has to name its own build.
target_compile_definitions(pico-thing-video PRIVATE
	BUILD_TYPE_STR="${CMAKE_BUILD_TYPE}"
	BUILD_CC_STR="${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")

if(NOT VIDEO_SCANOUT)
	target_compile_definitions(pico-thing-video PRIVATE VIDEO_SCANOUT_OFF=1)
	message(WARNING "VIDEO_SCANOUT=OFF: this image leaves the display DARK. "
	                "Measurement builds only.")
endif()

if(FAST_SYS_CLK_320)
	target_compile_definitions(pico-thing-video PRIVATE FAST_SYS_CLK_320=1)
endif()

# Include both the project headers and generated ones
target_include_directories(pico-thing-video
	PRIVATE
	${CMAKE_CURRENT_LIST_DIR}/include
	${CMAKE_CURRENT_BINARY_DIR}
)

pico_generate_pio_header(pico-thing-video ${CMAKE_CURRENT_LIST_DIR}/src/pico_thing_video.pio)

target_sources(pico-thing-video
	PRIVATE
		${FLEX_CmdLexer_OUTPUTS}
		${BISON_CmdParser_OUTPUTS}
)

# Core 1 stack is 0x800 (2 KiB) by default — bump it to leave headroom for
# nested ISR levels (DMA prio-0) with FPU lazy stacking.
#
# VIDEO_DEMO=1 draws the built-in test screen at boot (default on for now).
target_compile_definitions(pico-thing-video PRIVATE
	PICO_CORE1_STACK_SIZE=0x1000
	VIDEO_DEMO=1
)

target_link_libraries(pico-thing-video
	pico_stdlib
	pico_multicore
	pico_unique_id
	hardware_adc
	hardware_dma
	hardware_i2c
	hardware_irq
	hardware_pio
	hardware_psram
	hardware_pwm
	hardware_vreg
	hardware_watchdog
	tinyusb_board
)

# Link the TinyUSB stack matching the build-time role (see USB_HOST_MODE above).
if(USB_HOST_MODE)
	target_link_libraries(pico-thing-video tinyusb_host)
	target_compile_definitions(pico-thing-video PRIVATE USB_HOST_MODE=1)
else()
	target_link_libraries(pico-thing-video tinyusb_device)
endif()

# stdio never rides USB in either role (device: CDC is a separate virtual UART
# for the guest; host: no CDC at all). The console is always UART0.
pico_enable_stdio_usb(pico-thing-video 0)
pico_enable_stdio_uart(pico-thing-video 1)

pico_add_extra_outputs(pico-thing-video)

# Keep the merged clangd database in step with the build automatically.
#
# Neither build tree's compile_commands.json is complete on its own (each USB
# role compiles a source the other does not), so clangd is pointed at a merged
# union at the repo root. That union used to be produced by hand, which meant
# it silently described stale flags — or a source that no longer existed —
# until someone remembered to re-run the script.
#
# The refresh hangs off POST_BUILD rather than the configure step on purpose:
# CMake writes each tree's compile_commands.json while GENERATING, so anything
# run during configure would merge the previous run's data and defeat the
# point. Building is also what actually follows a reconfigure in practice.
#
# The tree being built is passed as the primary, so shared sources are
# described with the flags of whichever role was compiled last — matching what
# the developer is working on — and the script finds a tree supplying the other
# role by itself. This is a developer convenience, so a failure here (no jq, a
# read-only source tree) must never fail the build.
if(CMAKE_HOST_UNIX)
	add_custom_command(TARGET pico-thing-video POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E env
			${CMAKE_CURRENT_LIST_DIR}/scripts/merge-compile-commands.sh
			$<SHELL_PATH:${CMAKE_BINARY_DIR}> || ${CMAKE_COMMAND} -E true
		WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
		COMMENT "Refreshing merged compile_commands.json for clangd"
		VERBATIM)
endif()

# Run from flash/XIP rather than copying the whole image to SRAM: the ~495 KB
# video framebuffer needs that SRAM. The timing-critical bus code stays resident
# in RAM regardless, via __time_critical_func / __not_in_flash_func / __scratch.
# (Was: pico_set_binary_type(pico-thing-video copy_to_ram))
