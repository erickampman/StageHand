cmake_minimum_required(VERSION 3.21)
# macOS (docs/macos.md): set before project(), and ignored elsewhere.
set(CMAKE_OSX_DEPLOYMENT_TARGET 26.0 CACHE STRING "macOS 26 or later (docs/macos.md, decision 9)")
set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "arm64 only (docs/macos.md, decision 13)")
project(StageHand VERSION 0.9.0 LANGUAGES C CXX)   # C: DGL's pugl and NanoVG

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)    # for clangd

# Configure with Ninja:  cmake -B build -G Ninja
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# --- Platforms (docs/macos.md) -----------------------------------------------------
# Linux builds everything. macOS (milestones 3 to 6) builds the app over
# CoreAudio, CoreMIDI and CFBundle-loaded CLAP plugins, with Cocoa editor windows
# and the DGL rack window. STAGEHAND_APP=OFF builds the platform-free libraries
# and the test suites only, anywhere.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(stagehand_linux ON)
else()
    set(stagehand_linux OFF)
endif()
if(stagehand_linux OR APPLE)
    set(stagehand_app_default ON)
else()
    set(stagehand_app_default OFF)
endif()
option(STAGEHAND_APP "Build the app (Linux and macOS), not just the libraries and tests" ${stagehand_app_default})
if(STAGEHAND_APP AND NOT stagehand_app_default)
    message(FATAL_ERROR "The app builds on Linux and macOS only (docs/macos.md); configure with -DSTAGEHAND_APP=OFF")
endif()
if(STAGEHAND_APP AND stagehand_linux)
    set(stagehand_linux_app ON)
else()
    set(stagehand_linux_app OFF)
endif()

# --- System dependencies (docs/architecture.md, dependency summary) -----------
# X11 for the plugin editor windows, the Linux app's only. JACK (served by
# PipeWire under `pw-jack`) and ALSA's sequencer come in through uw-core.
find_package(Threads REQUIRED)
if(stagehand_linux_app)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(X11 REQUIRED IMPORTED_TARGET x11)   # plugin editor windows (M4)
    pkg_check_modules(XRANDR IMPORTED_TARGET xrandr)       # optional: the monitor a window opens on
endif()

# --- uw-core (git submodule): the code StageHand shares with Umpire ------------
# UMP, the clock and queues, the plugin seam and CLAP host (with the CLAP SDK,
# 1.2.10), the audio and MIDI backends; namespace uw (src/Core/UwNames.h). The
# backends (JACK and ALSA on Linux) only when building the app.
set(UW_CORE_BACKENDS ${STAGEHAND_APP} CACHE BOOL "" FORCE)
set(UW_CORE_TESTS OFF CACHE BOOL "" FORCE)
# STAGEHAND_UW_CORE_DIR points at another uw-core clone (e.g. ../uw-core) to build
# against a change that isn't pushed and pinned yet.
set(STAGEHAND_UW_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/uw-core" CACHE PATH "uw-core source")
add_subdirectory(${STAGEHAND_UW_CORE_DIR} uw-core)

# --- nlohmann/json 3.11.3: single header, vendored (same copy as PMU) ----------
add_library(nlohmann-json INTERFACE)
target_include_directories(nlohmann-json SYSTEM INTERFACE external)

# --- DPF's DGL (pugl + NanoVG over OpenGL): StageHand's own UI. The private DPF fork, a submodule
# (external/dpf/README.fork.md; Umpire's docs/decisions.md, "DPF fork"); only the DGL library,
# no plugin wrappers.
if(STAGEHAND_APP)
    if(APPLE)
        enable_language(OBJCXX)   # pugl's Cocoa backend (pugl.mm), and our own .mm files
    endif()
    set(DPF_LIBRARIES OFF CACHE BOOL "" FORCE)
    set(DPF_EXAMPLES OFF CACHE BOOL "" FORCE)
    add_subdirectory(external/dpf EXCLUDE_FROM_ALL)
    # Shared resources (the font); the file browser on macOS (decision 10:
    # Linux too, once DPF's portal path is patched as its macOS one is); no web view.
    # (APPLE is unset on Linux, so it can't be passed bare: it would expand to nothing.)
    if(APPLE)
        set(stagehand_dgl_file_browser TRUE)
    else()
        set(stagehand_dgl_file_browser FALSE)
    endif()
    dpf__add_dgl_opengl(TRUE ${stagehand_dgl_file_browser} FALSE)
    # StageHand's own namespace (docs/macos.md, milestone 5's notes): pugl names
    # its Objective-C classes after it, and every DPF plugin built with the
    # default ("DGL") registers the same ones, so a window of ours would collide
    # with the plugins it hosts. Harmless on Linux (C++ names only).
    target_compile_definitions(dgl-opengl PUBLIC DGL_NAMESPACE=StageHandDGL)
    # Its headers are third-party: no warnings from them in our code (like clap-sdk).
    foreach(dgl_target dgl-opengl dgl-opengl-definitions)
        get_target_property(dgl_includes ${dgl_target} INTERFACE_INCLUDE_DIRECTORIES)
        if(dgl_includes)
            set_target_properties(${dgl_target} PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${dgl_includes}")
        endif()
    endforeach()
    # pugl.hpp includes <cstdbool>, which GCC 15 flags with a #warning in C++17
    # and later. Third-party code (the fork keeps only needed patches): quiet it.
    # On macOS, pugl uses AppKit's pre-10.12 names (quieted, not patched).
    if(stagehand_linux)
        target_compile_options(dgl-opengl PRIVATE -Wno-cpp)
    else()
        target_compile_options(dgl-opengl PRIVATE -Wno-deprecated-declarations)
    endif()
endif()

# --- Warnings for our own code (not vendored/system headers) -------------------
add_library(stagehand-warnings INTERFACE)
target_compile_options(stagehand-warnings INTERFACE -Wall -Wextra -Wpedantic)

# --- stagehand-core: value models (headers), over uw-core's UMP, base and seam --
add_library(stagehand-core INTERFACE)
target_include_directories(stagehand-core INTERFACE src)
target_link_libraries(stagehand-core INTERFACE uw::base uw::plugin)
target_compile_definitions(stagehand-core INTERFACE STAGEHAND_VERSION="${PROJECT_VERSION}")

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
    src/Session/AudioDeviceState.cpp
    src/Session/RackFile.cpp
    src/Session/RigFile.cpp
    src/Session/Session.cpp
    src/Session/SetFile.cpp
    src/Session/SetlistFile.cpp
    src/Session/WindowState.cpp)
target_link_libraries(stagehand-session
    PUBLIC  stagehand-engine
    PRIVATE nlohmann-json stagehand-warnings)

# --- stagehand-app: the rack view's controller and model (no DGL, X11 or JACK) ---
add_library(stagehand-app STATIC
    src/App/BeatNow.cpp
    src/App/FileDialog.cpp
    src/App/ParameterEditor.cpp
    src/App/RackController.cpp
    src/App/SequenceEditor.cpp)
target_link_libraries(stagehand-app
    PUBLIC  stagehand-session
    PRIVATE stagehand-warnings)

# --- stagehand-ui: the rack window (DGL), both platforms. Only this library sees DGL.
if(STAGEHAND_APP)
    # The toolbar's brand, embedded (resources/README.md says where each came from).
    include(cmake/EmbedFile.cmake)
    embed_file("${CMAKE_SOURCE_DIR}/resources/brand/Unlikelyware-logo-144.png"
               "${CMAKE_BINARY_DIR}/BrandLogo.cpp" kBrandLogo)
    embed_file("${CMAKE_SOURCE_DIR}/resources/fonts/Karla-Regular.ttf"
               "${CMAKE_BINARY_DIR}/BrandFont.cpp" kBrandFont)
    add_library(stagehand-ui STATIC
        src/Ui/RackUi.cpp
        src/Ui/RackWidget.cpp
        src/Ui/UiWidget.cpp
        src/Ui/SequenceEditorWidget.cpp
        src/Ui/ParameterEditorWidget.cpp
        "${CMAKE_BINARY_DIR}/BrandLogo.cpp"
        "${CMAKE_BINARY_DIR}/BrandFont.cpp")
    target_link_libraries(stagehand-ui
        PUBLIC  stagehand-app
        PRIVATE dgl-opengl dgl-opengl-definitions stagehand-warnings)
    if(APPLE)
        target_sources(stagehand-ui PRIVATE src/Ui/MonitorMac.cpp src/Ui/FileChooser.cpp)
        target_link_libraries(stagehand-ui PRIVATE "-framework CoreGraphics")
        target_link_libraries(dgl-opengl PRIVATE "-framework UniformTypeIdentifiers")   # the browser's filter
    else()
        # The windows' icon (_NET_WM_ICON), embedded: tools/render-linux-icons.sh made them.
        embed_file("${CMAKE_SOURCE_DIR}/resources/linux/icons/stagehand-128.png"
                   "${CMAKE_BINARY_DIR}/WindowIcon128.cpp" kWindowIcon128)
        embed_file("${CMAKE_SOURCE_DIR}/resources/linux/icons/stagehand-48.png"
                   "${CMAKE_BINARY_DIR}/WindowIcon48.cpp" kWindowIcon48)
        target_sources(stagehand-ui PRIVATE src/Ui/Monitor.cpp src/Ui/StartupId.cpp src/Ui/WindowIcon.cpp
                       "${CMAKE_BINARY_DIR}/WindowIcon128.cpp" "${CMAKE_BINARY_DIR}/WindowIcon48.cpp")
        target_link_libraries(stagehand-ui PRIVATE PkgConfig::X11)
        if(XRANDR_FOUND)
            target_compile_definitions(stagehand-ui PRIVATE STAGEHAND_HAVE_XRANDR)
            target_link_libraries(stagehand-ui PRIVATE PkgConfig::XRANDR)
        else()
            message(STATUS "xrandr not found: a new window fits the whole X screen, not its monitor")
        endif()
    endif()
endif()

# --- The Linux seams and the app (STAGEHAND_APP on Linux) ------------------
if(stagehand_linux_app)

    # --- stagehand-lib: the seam implementations (uw-core's JACK, ALSA and CLAP)
    add_library(stagehand-lib INTERFACE)
    target_link_libraries(stagehand-lib INTERFACE stagehand-engine uw::audio uw::midi uw::clap-host)

    # --- The app --------------------------------------------------------------------
    add_executable(stagehand
        src/App/main.cpp
        src/App/AppHost.cpp
        src/App/EditorWindow.cpp
        src/App/MainLoop.cpp
        src/App/MidiMonitor.cpp)
    target_link_libraries(stagehand PRIVATE stagehand-lib stagehand-app stagehand-ui PkgConfig::X11 stagehand-warnings)

endif()

# --- The macOS seams and the app (STAGEHAND_APP on macOS) -----------------------------
if(STAGEHAND_APP AND APPLE)

    # The seam implementations: uw-core's CoreAudio, CoreMIDI and CLAP.
    add_library(stagehand-lib INTERFACE)
    target_link_libraries(stagehand-lib INTERFACE stagehand-engine uw::audio uw::midi uw::clap-host)

    add_executable(stagehand
        src/App/main.cpp
        src/App/AppDelegateMac.mm
        src/App/AppHost.cpp
        src/App/EditorWindowMac.mm
        src/App/MainLoopMac.mm
        src/App/MidiMonitor.cpp)
    set_source_files_properties(src/App/AppDelegateMac.mm src/App/EditorWindowMac.mm src/App/MainLoopMac.mm
                                PROPERTIES COMPILE_OPTIONS -fobjc-arc)
    target_link_libraries(stagehand PRIVATE stagehand-lib stagehand-ui "-framework AppKit" stagehand-warnings)

    # StageHand.app (docs/macos.md, "The app bundle"): the Info.plist declares
    # HiDPI and the .rack (a package) and .setlist document types.
    set_target_properties(stagehand PROPERTIES
        OUTPUT_NAME                        StageHand
        MACOSX_BUNDLE                      TRUE
        MACOSX_BUNDLE_INFO_PLIST           "${CMAKE_SOURCE_DIR}/cmake/Info.plist.in"
        MACOSX_BUNDLE_BUNDLE_NAME          StageHand
        MACOSX_BUNDLE_GUI_IDENTIFIER       com.unlikelyware.stagehand
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_BUNDLE_VERSION       "${PROJECT_VERSION}")
    # The icon, compiled from resources/StageHand.icon (an Icon Composer document)
    # by Xcode's actool: Assets.car, which macOS 26 draws (its dark and tinted
    # versions too), and StageHand.icns, which actool still writes. Without Xcode
    # (only the Command Line Tools) the bundle has no icon. Nor with an Xcode
    # before 26: Icon Composer documents are new in 26, and an older actool
    # reports success and writes neither file.
    execute_process(COMMAND xcrun --find actool OUTPUT_VARIABLE STAGEHAND_ACTOOL
                    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE actool_missing)
    set(actool_version 0)
    if(NOT actool_missing)
        execute_process(COMMAND "${STAGEHAND_ACTOOL}" --version OUTPUT_VARIABLE actool_info ERROR_QUIET)
        if(actool_info MATCHES "<key>short-bundle-version</key>[ \t\r\n]*<string>([0-9.]+)</string>")
            set(actool_version "${CMAKE_MATCH_1}")
        endif()
    endif()
    if(actool_missing)
        message(WARNING "Xcode's actool not found: StageHand.app will have no icon "
                        "(install Xcode, not just the Command Line Tools)")
    elseif(actool_version VERSION_LESS 26)
        message(WARNING "Xcode ${actool_version}'s actool can't compile Icon Composer documents: "
                        "StageHand.app will have no icon (Xcode 26 or later makes it)")
    else()
        set(icon "${CMAKE_SOURCE_DIR}/resources/StageHand.icon")
        set(icon_out "${CMAKE_BINARY_DIR}/icon")
        file(GLOB_RECURSE icon_inputs CONFIGURE_DEPENDS "${icon}/*")
        add_custom_command(
            OUTPUT "${icon_out}/Assets.car" "${icon_out}/StageHand.icns"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${icon_out}"
            COMMAND "${STAGEHAND_ACTOOL}" "${icon}" --compile "${icon_out}" --app-icon StageHand
                    --platform macosx --target-device mac
                    --minimum-deployment-target "${CMAKE_OSX_DEPLOYMENT_TARGET}"
                    --output-partial-info-plist "${icon_out}/partial.plist"
                    --errors --warnings --output-format human-readable-text
            DEPENDS ${icon_inputs}
            COMMENT "Compiling the app icon"
            VERBATIM)
        set(icon_files "${icon_out}/Assets.car" "${icon_out}/StageHand.icns")
        target_sources(stagehand PRIVATE ${icon_files})
        set_source_files_properties(${icon_files} PROPERTIES GENERATED TRUE MACOSX_PACKAGE_LOCATION Resources)
    endif()
    # Ad-hoc signed once whole: the linker signs only the executable, not the
    # Resources added after it (milestone 7 signs it properly).
    add_custom_command(TARGET stagehand POST_BUILD
        COMMAND codesign --force --sign - "$<TARGET_BUNDLE_DIR:stagehand>"
        VERBATIM)
    # build/stagehand stays the command line's: a script running the bundle's
    # binary by its real path. Not a link: run through one, the binary isn't
    # seen as the bundle's (no bundle id, name or Info.plist).
    file(REMOVE "${CMAKE_BINARY_DIR}/stagehand")   # a link, from before: don't write through it
    file(CONFIGURE OUTPUT "${CMAKE_BINARY_DIR}/stagehand" @ONLY CONTENT
"#!/bin/sh\nexec \"@CMAKE_BINARY_DIR@/StageHand.app/Contents/MacOS/StageHand\" \"$@\"\n")
    file(CHMOD "${CMAKE_BINARY_DIR}/stagehand"
         PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)

endif()

# --- The help page (both platforms) -----------------------------------------------
# resources/help/help.html, exported from help.md (beside it) by Markdown Web
# Writer, and the logo it shows, as Unlikelyware.png: copied to build/help,
# beside the executable, where Help (the toolbar's "?", the File menu) looks on
# Linux; on the Mac they also go in the bundle's Resources/help.
if(TARGET stagehand)
    set(help_files "${CMAKE_BINARY_DIR}/help/help.html" "${CMAKE_BINARY_DIR}/help/Unlikelyware.png")
    configure_file(resources/help/help.html "${CMAKE_BINARY_DIR}/help/help.html" COPYONLY)
    configure_file(resources/brand/Unlikelyware-logo-144.png "${CMAKE_BINARY_DIR}/help/Unlikelyware.png" COPYONLY)
    if(APPLE)
        target_sources(stagehand PRIVATE ${help_files})
        set_source_files_properties(${help_files} PROPERTIES MACOSX_PACKAGE_LOCATION Resources/help)
    endif()

    # Linux: `cmake --install build` (to /usr/local, or --prefix), as KeyboardEDO's:
    # the executable in bin, the help page in share/stagehand/help (where Help
    # looks, from bin), and a launcher and the icons where desktops find them.
    # The launcher runs `stagehand` from the PATH; tools/install-desktop-entry.sh
    # is for running a checkout's build/stagehand instead.
    if(UNIX AND NOT APPLE)
        include(GNUInstallDirs)
        set(EXEC stagehand)
        configure_file(resources/linux/stagehand.desktop.in "${CMAKE_BINARY_DIR}/stagehand.desktop" @ONLY)
        install(TARGETS stagehand RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
        install(FILES ${help_files} DESTINATION ${CMAKE_INSTALL_DATADIR}/stagehand/help)
        install(FILES "${CMAKE_BINARY_DIR}/stagehand.desktop" DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
        foreach(size 48 64 128 256 512)
            install(FILES resources/linux/icons/stagehand-${size}.png
                    DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${size}x${size}/apps
                    RENAME stagehand.png)
        endforeach()
    endif()
endif()

# --- Tests (tests/Check.h; one executable per suite, no third-party framework) ---
# The UMP, queue, clock and denormal tests moved to uw-core with their code.
enable_testing()
function(stagehand_add_test NAME SOURCE LIBRARY)
    add_executable(${NAME} ${SOURCE})
    target_include_directories(${NAME} PRIVATE tests)
    target_link_libraries(${NAME} PRIVATE ${LIBRARY} stagehand-warnings Threads::Threads)
    add_test(NAME ${NAME} COMMAND ${NAME})
endfunction()
stagehand_add_test(test-engine tests/EngineTests.cpp    stagehand-engine)
stagehand_add_test(test-sequence tests/SequenceTests.cpp stagehand-engine)
stagehand_add_test(test-looper  tests/LooperTests.cpp    stagehand-engine)
stagehand_add_test(test-rackfile tests/RackFileTests.cpp stagehand-session)
stagehand_add_test(test-setlistfile tests/SetlistFileTests.cpp stagehand-session)
stagehand_add_test(test-setfile tests/SetFileTests.cpp stagehand-session)
stagehand_add_test(test-rigfile tests/RigFileTests.cpp stagehand-session)
stagehand_add_test(test-session  tests/SessionTests.cpp  stagehand-session)
stagehand_add_test(test-rackcontroller tests/RackControllerTests.cpp stagehand-app)
stagehand_add_test(test-sequenceeditor tests/SequenceEditorTests.cpp stagehand-app)
stagehand_add_test(test-beatnow tests/BeatNowTests.cpp stagehand-app)
stagehand_add_test(test-filedialog tests/FileDialogTests.cpp stagehand-app)
if(TARGET stagehand-lib)   # real plugins: a note effect in front of an instrument (skips unless installed)
    stagehand_add_test(test-note-effects tests/NoteEffectTests.cpp stagehand-lib)
endif()
