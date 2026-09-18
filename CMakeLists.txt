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
option(USB_HOST_MODE "Build USB as HID host (keyboard+mouse) instead of CDC device" OFF)

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
		src/pico_thing_video.cpp
		src/power.cpp
		src/i2c_service.cpp
		src/registers.cpp
		src/usb.cpp
		src/video.cpp
)

# USB role-specific sources: the device build needs its descriptors; the host
# build needs the HID/hub callbacks + input cooking instead (and never uses the
# device descriptors).
if(USB_HOST_MODE)
	target_sources(pico-thing-video PRIVATE src/usb_host.cpp)
else()
	target_sources(pico-thing-video PRIVATE src/usb_descriptors.cpp)
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
	pico_aon_timer
	pico_unique_id
	hardware_adc
	hardware_dma
	hardware_i2c
	hardware_irq
	hardware_pio
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

# Run from flash/XIP rather than copying the whole image to SRAM: the ~495 KB
# video framebuffer needs that SRAM. The timing-critical bus code stays resident
# in RAM regardless, via __time_critical_func / __not_in_flash_func / __scratch.
# (Was: pico_set_binary_type(pico-thing-video copy_to_ram))
