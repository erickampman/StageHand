cmake_minimum_required(VERSION 3.21)
# macOS (docs/macos.md): set before project(), and ignored elsewhere.
set(CMAKE_OSX_DEPLOYMENT_TARGET 15.0 CACHE STRING "macOS 15 or later (docs/macos.md, decision 9)")
set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "arm64 only (docs/macos.md, decision 13)")
project(StageHand VERSION 0.0.1 LANGUAGES C CXX)   # C: DGL's pugl and NanoVG

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)    # for clangd

# Configure with Ninja:  cmake -B build -G Ninja
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# --- Platforms (docs/macos.md) -----------------------------------------------------
# Linux builds everything. Elsewhere (macOS, so far: milestone 1) only the
# platform-free libraries and the test suites build; the app needs backends
# that aren't written yet. STAGEHAND_APP=OFF does the same on Linux.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(stagehand_linux ON)
else()
    set(stagehand_linux OFF)
endif()
option(STAGEHAND_APP "Build the app (Linux only, for now), not just the libraries and tests" ${stagehand_linux})
if(STAGEHAND_APP AND NOT stagehand_linux)
    message(FATAL_ERROR "The app builds on Linux only, so far (docs/macos.md); configure with -DSTAGEHAND_APP=OFF")
endif()

# --- System dependencies (docs/architecture.md, dependency summary) -----------
# JACK API headers from libjack-jackd2-dev; at runtime the calls are served by
# PipeWire (pipewire-jack) — run under `pw-jack`. ALSA's sequencer (UMP mode,
# alsa-lib 1.2.10+) provides MIDI input. The app's only.
find_package(Threads REQUIRED)
if(STAGEHAND_APP)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(JACK REQUIRED IMPORTED_TARGET jack)
    pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa>=1.2.10)
    pkg_check_modules(X11 REQUIRED IMPORTED_TARGET x11)   # plugin editor windows (M4)
    pkg_check_modules(XRANDR IMPORTED_TARGET xrandr)       # optional: the monitor a window opens on
endif()

# --- CLAP SDK: header-only, vendored submodule pinned to PMU's tag (1.2.10) ----
add_library(clap-sdk INTERFACE)
target_include_directories(clap-sdk SYSTEM INTERFACE external/clap/include)

# --- nlohmann/json 3.11.3: single header, vendored (same copy as PMU) ----------
add_library(nlohmann-json INTERFACE)
target_include_directories(nlohmann-json SYSTEM INTERFACE external)

# --- DPF's DGL (pugl + NanoVG over OpenGL): StageHand's own UI. Vendored from PMU
# (external/dpf/README.StageHand.md); only the DGL library, no plugin wrappers.
# The app's only.
if(STAGEHAND_APP)
    set(DPF_LIBRARIES OFF CACHE BOOL "" FORCE)
    set(DPF_EXAMPLES OFF CACHE BOOL "" FORCE)
    add_subdirectory(external/dpf EXCLUDE_FROM_ALL)
    dpf__add_dgl_opengl(TRUE FALSE FALSE)   # shared resources (the font); no file browser, no web view
    # Its headers are third-party: no warnings from them in our code (like clap-sdk).
    foreach(dgl_target dgl-opengl dgl-opengl-definitions)
        get_target_property(dgl_includes ${dgl_target} INTERFACE_INCLUDE_DIRECTORIES)
        if(dgl_includes)
            set_target_properties(${dgl_target} PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${dgl_includes}")
        endif()
    endforeach()
endif()

# --- Warnings for our own code (not vendored/system headers) -------------------
add_library(stagehand-warnings INTERFACE)
target_compile_options(stagehand-warnings INTERFACE -Wall -Wextra -Wpedantic)

# --- stagehand-core: value models, UMP helpers, seam interfaces ----------------
add_library(stagehand-core STATIC
    src/Core/Clock.cpp
    src/Core/Ump.cpp)
target_include_directories(stagehand-core PUBLIC src)
target_link_libraries(stagehand-core PRIVATE stagehand-warnings)

# --- stagehand-engine: the graph runner (no platform or format dependencies) ---
add_library(stagehand-engine STATIC
    src/Engine/Engine.cpp
    src/Engine/EngineGraph.cpp
    src/Engine/SequenceSet.cpp
    src/Engine/Looper.cpp
    src/Engine/ControlLink.cpp)
target_link_libraries(stagehand-engine
    PUBLIC  stagehand-core
    PRIVATE stagehand-warnings)

# --- stagehand-session: racks (files) and running them (no platform deps) -------
add_library(stagehand-session STATIC
    src/Session/AppPaths.cpp
    src/Session/RackFile.cpp
    src/Session/RigFile.cpp
    src/Session/Session.cpp
    src/Session/SetlistFile.cpp
    src/Session/WindowState.cpp)
target_link_libraries(stagehand-session
    PUBLIC  stagehand-engine
    PRIVATE nlohmann-json stagehand-warnings)

# --- stagehand-app: the rack view's controller and model (no DGL, X11 or JACK) ---
add_library(stagehand-app STATIC
    src/App/BeatNow.cpp
    src/App/FileDialog.cpp
    src/App/RackController.cpp
    src/App/SequenceEditor.cpp)
target_link_libraries(stagehand-app
    PUBLIC  stagehand-session
    PRIVATE stagehand-warnings)

# --- The Linux seams, the UI and the app (STAGEHAND_APP) ---------------------------
if(STAGEHAND_APP)

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

    # --- stagehand-ui: the rack window (DGL). Only this library sees DGL. -------------
    add_library(stagehand-ui STATIC
        src/Ui/RackUi.cpp
        src/Ui/RackWidget.cpp
        src/Ui/UiWidget.cpp
        src/Ui/SequenceEditorWidget.cpp
        src/Ui/Monitor.cpp)
    target_link_libraries(stagehand-ui
        PUBLIC  stagehand-app
        PRIVATE dgl-opengl dgl-opengl-definitions PkgConfig::X11 stagehand-warnings)
    if(XRANDR_FOUND)
        target_compile_definitions(stagehand-ui PRIVATE STAGEHAND_HAVE_XRANDR)
        target_link_libraries(stagehand-ui PRIVATE PkgConfig::XRANDR)
    else()
        message(STATUS "xrandr not found: a new window fits the whole X screen, not its monitor")
    endif()

    # --- The app --------------------------------------------------------------------
    add_executable(stagehand
        src/App/main.cpp
        src/App/AppHost.cpp
        src/App/EditorWindow.cpp
        src/App/MainLoop.cpp
        src/App/MidiMonitor.cpp)
    target_link_libraries(stagehand PRIVATE stagehand-lib stagehand-app stagehand-ui PkgConfig::X11 stagehand-warnings)

endif()

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
stagehand_add_test(test-clock  tests/ClockTests.cpp     stagehand-core)
stagehand_add_test(test-denormals tests/DenormalTests.cpp stagehand-core)
stagehand_add_test(test-engine tests/EngineTests.cpp    stagehand-engine)
stagehand_add_test(test-sequence tests/SequenceTests.cpp stagehand-engine)
stagehand_add_test(test-looper  tests/LooperTests.cpp    stagehand-engine)
stagehand_add_test(test-rackfile tests/RackFileTests.cpp stagehand-session)
stagehand_add_test(test-setlistfile tests/SetlistFileTests.cpp stagehand-session)
stagehand_add_test(test-rigfile tests/RigFileTests.cpp stagehand-session)
stagehand_add_test(test-session  tests/SessionTests.cpp  stagehand-session)
stagehand_add_test(test-rackcontroller tests/RackControllerTests.cpp stagehand-app)
stagehand_add_test(test-sequenceeditor tests/SequenceEditorTests.cpp stagehand-app)
stagehand_add_test(test-beatnow tests/BeatNowTests.cpp stagehand-app)
stagehand_add_test(test-filedialog tests/FileDialogTests.cpp stagehand-app)
