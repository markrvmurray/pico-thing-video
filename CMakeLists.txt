cmake_minimum_required(VERSION 3.14)  # 3.14+ for BISON_TARGET DEFINES_FILE
# Development board (has the HDMI/DVI breakout for bringing up the video engine).
# Swap back to pimoroni_pga2350 for roll-out.
#set(PICO_BOARD pimoroni_pga2350)
set(PICO_BOARD solderparty_rp2350_stamp_xl_carrier)

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
		src/pico_thing_video.cpp
		src/registers.cpp
		src/usb_descriptors.cpp
		src/usb.cpp
		src/video.cpp
)

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
	pico_aon_timer
	pico_unique_id
	hardware_dma
	hardware_irq
	hardware_pio
	hardware_vreg
	tinyusb_device
	tinyusb_board
)

pico_enable_stdio_usb(pico-thing-video 0)
pico_enable_stdio_uart(pico-thing-video 1)

pico_add_extra_outputs(pico-thing-video)

# Run from flash/XIP rather than copying the whole image to SRAM: the ~495 KB
# video framebuffer needs that SRAM. The timing-critical bus code stays resident
# in RAM regardless, via __time_critical_func / __not_in_flash_func / __scratch.
# (Was: pico_set_binary_type(pico-thing-video copy_to_ram))
