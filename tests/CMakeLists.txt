cmake_minimum_required(VERSION 3.14)
project(pico-thing-video-tests CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD 11)

# Keep build trees out of Time Machine: they churn on every build and can be
# regenerated.  A sticky exclusion (no -p) is stored on the directory itself,
# so unlike a fixed-path one it needs no root; it goes if the directory is
# deleted and comes back at the next configure.
# CMAKE_HOST_APPLE to match the firmware build, where APPLE is false.
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

# macOS ships bison 2.3 which is too old; use Homebrew bison 3.x.
if(APPLE)
    find_program(_bison_homebrew
        NAMES bison
        PATHS /opt/homebrew/opt/bison/bin /usr/local/opt/bison/bin
        NO_DEFAULT_PATH
    )
    if(_bison_homebrew)
        set(BISON_EXECUTABLE "${_bison_homebrew}" CACHE FILEPATH "Bison executable" FORCE)
    endif()
endif()
find_package(FLEX  REQUIRED)
find_package(BISON 3.0 REQUIRED)

include(FetchContent)
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.7.1
)
FetchContent_MakeAvailable(Catch2)

# Catch2's extra CMake modules supply catch_discover_tests()
list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(CTest)
include(Catch)

# ---------------------------------------------------------------------------
# test_command_parser: the flex/bison command parser compiled natively for the
# host.
#
# Re-runs flex/bison on the same source files used by the firmware build; the
# generated .c files land in the tests binary directory and are compiled with
# the host compiler (not arm-none-eabi-gcc).  cmd_parse_shim.c supplies a
# host copy of cmd_parse() that mirrors the firmware driver.
# ---------------------------------------------------------------------------
FLEX_TARGET(CmdLexerTest
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/command_lexer.l
    ${CMAKE_CURRENT_BINARY_DIR}/command_lexer.c
)
BISON_TARGET(CmdParserTest
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/command_parser.y
    ${CMAKE_CURRENT_BINARY_DIR}/command_parser.c
    DEFINES_FILE ${CMAKE_CURRENT_BINARY_DIR}/command_parser.tab.h
)
ADD_FLEX_BISON_DEPENDENCY(CmdLexerTest CmdParserTest)

add_executable(test_command_parser
    test_command_parser.cpp
    cmd_parse_shim.c
    ${FLEX_CmdLexerTest_OUTPUTS}
    ${BISON_CmdParserTest_OUTPUTS}
)
target_include_directories(test_command_parser PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_BINARY_DIR}
)
target_link_libraries(test_command_parser PRIVATE Catch2::Catch2WithMain)
catch_discover_tests(test_command_parser)
