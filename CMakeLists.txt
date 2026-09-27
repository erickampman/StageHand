cmake_minimum_required(VERSION 3.21)
project(StageHand VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)    # for clangd

# Configure with Ninja:  cmake -B build -G Ninja
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# --- System dependencies (docs/architecture.md, dependency summary) -----------
# JACK API headers from libjack-jackd2-dev; at runtime the calls are served by
# PipeWire (pipewire-jack) — run under `pw-jack`. ALSA joins in M2 (MIDI input).
find_package(PkgConfig REQUIRED)
pkg_check_modules(JACK REQUIRED IMPORTED_TARGET jack)

# --- CLAP SDK: header-only, vendored submodule pinned to PMU's tag (1.2.10) ----
add_library(clap-sdk INTERFACE)
target_include_directories(clap-sdk INTERFACE external/clap/include)

# --- Warnings for our own code (not vendored/system headers) -------------------
add_library(stagehand-warnings INTERFACE)
target_compile_options(stagehand-warnings INTERFACE -Wall -Wextra -Wpedantic)

# --- stagehand-core: value models, UMP helpers, seam interfaces ----------------
# Header-only for now; becomes a static library when Core/Ump.cpp lands (M5).
add_library(stagehand-core INTERFACE)
target_include_directories(stagehand-core INTERFACE src)

# --- stagehand-lib: seam implementations (everything except main) --------------
add_library(stagehand-lib STATIC
    src/Audio/JackBackend.cpp)
target_link_libraries(stagehand-lib
    PUBLIC  stagehand-core PkgConfig::JACK
    PRIVATE stagehand-warnings)

# --- The app --------------------------------------------------------------------
add_executable(stagehand src/App/main.cpp)
target_link_libraries(stagehand PRIVATE stagehand-lib stagehand-warnings)

# --- Tests (self-contained runner; no third-party framework) --------------------
enable_testing()
add_executable(stagehand-tests tests/UmpTests.cpp)
target_link_libraries(stagehand-tests PRIVATE stagehand-core stagehand-warnings)
add_test(NAME ump COMMAND stagehand-tests)
