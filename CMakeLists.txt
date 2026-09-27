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
# PipeWire (pipewire-jack) — run under `pw-jack`. ALSA's sequencer (UMP mode,
# alsa-lib 1.2.10+) provides MIDI input.
find_package(PkgConfig REQUIRED)
pkg_check_modules(JACK REQUIRED IMPORTED_TARGET jack)
pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa>=1.2.10)
pkg_check_modules(X11 REQUIRED IMPORTED_TARGET x11)   # plugin editor windows (M4)
find_package(Threads REQUIRED)

# --- CLAP SDK: header-only, vendored submodule pinned to PMU's tag (1.2.10) ----
add_library(clap-sdk INTERFACE)
target_include_directories(clap-sdk SYSTEM INTERFACE external/clap/include)

# --- nlohmann/json 3.11.3: single header, vendored (same copy as PMU) ----------
add_library(nlohmann-json INTERFACE)
target_include_directories(nlohmann-json SYSTEM INTERFACE external)

# --- Warnings for our own code (not vendored/system headers) -------------------
add_library(stagehand-warnings INTERFACE)
target_compile_options(stagehand-warnings INTERFACE -Wall -Wextra -Wpedantic)

# --- stagehand-core: value models, UMP helpers, seam interfaces ----------------
add_library(stagehand-core STATIC
    src/Core/Ump.cpp)
target_include_directories(stagehand-core PUBLIC src)
target_link_libraries(stagehand-core PRIVATE stagehand-warnings)

# --- stagehand-engine: the graph runner (no platform or format dependencies) ---
add_library(stagehand-engine STATIC
    src/Engine/Engine.cpp
    src/Engine/EngineGraph.cpp)
target_link_libraries(stagehand-engine
    PUBLIC  stagehand-core
    PRIVATE stagehand-warnings)

# --- stagehand-session: racks (files) and running them (no platform deps) -------
add_library(stagehand-session STATIC
    src/Session/RackFile.cpp
    src/Session/Session.cpp)
target_link_libraries(stagehand-session
    PUBLIC  stagehand-engine
    PRIVATE nlohmann-json stagehand-warnings)

# --- stagehand-lib: seam implementations (everything except main) --------------
add_library(stagehand-lib STATIC
    src/Audio/JackBackend.cpp
    src/Midi/AlsaSeqInput.cpp
    src/Plugin/Clap/ClapModule.cpp
    src/Plugin/Clap/ClapPlugin.cpp
    src/Plugin/Clap/ClapPluginFactory.cpp)
target_link_libraries(stagehand-lib
    PUBLIC  stagehand-engine PkgConfig::JACK PkgConfig::ALSA Threads::Threads ${CMAKE_DL_LIBS}
    PRIVATE clap-sdk stagehand-warnings)

# --- The app --------------------------------------------------------------------
add_executable(stagehand
    src/App/main.cpp
    src/App/AppHost.cpp
    src/App/EditorWindow.cpp
    src/App/MainLoop.cpp
    src/App/MidiMonitor.cpp)
target_link_libraries(stagehand PRIVATE stagehand-lib stagehand-session PkgConfig::X11 stagehand-warnings)

# --- Tests (tests/Check.h; one executable per suite, no third-party framework) ---
enable_testing()
function(stagehand_add_test NAME SOURCE LIBRARY)
    add_executable(${NAME} ${SOURCE})
    target_include_directories(${NAME} PRIVATE tests)
    target_link_libraries(${NAME} PRIVATE ${LIBRARY} stagehand-warnings Threads::Threads)
    add_test(NAME ${NAME} COMMAND ${NAME})
endfunction()
stagehand_add_test(test-ump    tests/UmpTests.cpp       stagehand-core)
stagehand_add_test(test-spsc   tests/SpscQueueTests.cpp stagehand-core)
stagehand_add_test(test-engine tests/EngineTests.cpp    stagehand-engine)
stagehand_add_test(test-rackfile tests/RackFileTests.cpp stagehand-session)
stagehand_add_test(test-session  tests/SessionTests.cpp  stagehand-session)
