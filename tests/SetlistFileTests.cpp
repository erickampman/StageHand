// Setlist file tests: the round trip, racks written relative to the setlist's
// folder (and absolute when they share only the root), a missing rack kept
// with a warning, the switch settings, failures, path helpers, and the last
// position. Files go in a scratch folder under /tmp.

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "Check.h"
#include "Session/AppPaths.h"
#include "Session/RackFile.h"
#include "Session/SetlistFile.h"
#include "Session/WindowState.h"

using namespace stagehand;
namespace fs = std::filesystem;

namespace {

fs::path scratch() {
    const fs::path dir = fs::temp_directory_path() / ("stagehand-setlistfile-" + std::to_string(getpid()));
    fs::create_directories(dir);
    return fs::canonical(dir);
}

void writeText(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path) << text;
}

std::string readText(const fs::path& path) {
    std::ifstream in(path);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void makeRack(const fs::path& bundle) {
    Rack rack;
    rack.name = bundle.stem().string();
    CHECK(saveRack(bundle.string(), rack).ok);
}

void testRoundTrip(const fs::path& dir) {
    const fs::path sets = dir / "Gigs";
    makeRack(dir / "Racks/Autumn leaves.rack");
    makeRack(dir / "Racks/So what.rack");

    Setlist set;
    set.songs = {{(dir / "Racks/Autumn leaves.rack").string(), ""},
                 {(dir / "Racks/So what.rack").string(), "So what (fast)"},
                 {(dir / "Racks/Autumn leaves.rack").string(), ""}};   // a reprise
    set.switching.when = SwitchSettings::When::NextBar;
    set.rig            = (dir / "Stage.rig").string();
    SetlistTrigger next;   // a pedal on a device
    next.source  = MidiSourceRef{MidiSourceRef::Kind::Device, "FCB1010:FCB1010 MIDI 1"};
    next.control = SequenceTrigger::Control::Controller;
    next.number  = 80;
    SetlistTrigger song;   // Program Change, any input, channel 16
    song.action   = SetlistTrigger::Action::Song;
    song.channel  = 15;
    set.triggers  = {next, song};
    const fs::path file = sets / "Friday.setlist";
    CHECK(saveSetlist(file.string(), set).ok);
    CHECK(!fs::exists(file.string() + ".tmp"));

    // Relative to the setlist's folder, so moving both together keeps working.
    const std::string text = readText(file);
    CHECK(text.find("\"../Racks/So what.rack\"") != std::string::npos);
    CHECK(text.find("\"nextBar\"") != std::string::npos && text.find("\"ringOut\"") != std::string::npos);
    CHECK(text.find("\"rig\": \"../Stage.rig\"") != std::string::npos);   // relative, like the racks
    CHECK(text.find("\"action\": \"song\"") != std::string::npos && text.find("\"controller\": 80") != std::string::npos);

    Setlist loaded;
    const RackFileResult result = loadSetlist(file.string(), loaded);
    CHECK(result.ok && result.warnings.empty());
    CHECK(loaded == set);

    // Defaults aren't written.
    Setlist plain;
    plain.songs = {set.songs[0]};
    CHECK(saveSetlist(file.string(), plain).ok);
    CHECK(readText(file).find("switching") == std::string::npos);
    CHECK(loadSetlist(file.string(), loaded).ok && loaded == plain);
}

void testMissingAndAbsolute(const fs::path& dir) {
    // A missing rack is kept, with a warning.
    writeText(dir / "Old.setlist", R"({ "format": "stagehand-setlist", "version": 1,
        "songs": [ { "rack": "Racks/Autumn leaves.rack" }, { "rack": "Gone.rack", "name": "Gone" } ] })");
    Setlist set;
    const RackFileResult result = loadSetlist((dir / "Old.setlist").string(), set);
    CHECK(result.ok && set.songs.size() == 2 && result.warnings.size() == 1);
    if (set.songs.size() == 2) {
        CHECK(set.songs[0].rack == (dir / "Racks/Autumn leaves.rack").string());
        CHECK(set.songs[1].rack == (dir / "Gone.rack").string() && set.songs[1].name == "Gone");
    }

    // Sharing only "/" with the setlist: written absolute.
    Setlist far;
    far.songs = {{"/opt/racks/Organ.rack", ""}};
    CHECK(saveSetlist((dir / "Far.setlist").string(), far).ok);
    CHECK(readText(dir / "Far.setlist").find("\"/opt/racks/Organ.rack\"") != std::string::npos);
    Setlist back;
    CHECK(loadSetlist((dir / "Far.setlist").string(), back).ok && back == far);
}

void testFailures(const fs::path& dir) {
    Setlist set;
    CHECK(!loadSetlist((dir / "Nothing.setlist").string(), set).ok);
    writeText(dir / "Rack.setlist", R"({ "format": "stagehand-rack", "version": 1 })");
    CHECK(!loadSetlist((dir / "Rack.setlist").string(), set).ok);
    writeText(dir / "New.setlist", R"({ "format": "stagehand-setlist", "version": 9 })");
    const RackFileResult newer = loadSetlist((dir / "New.setlist").string(), set);
    CHECK(!newer.ok && newer.error.find("newer") != std::string::npos);
    writeText(dir / "Bad.setlist", R"({ "format": "stagehand-setlist", "version": 1, "songs": [ {} ] })");
    CHECK(!loadSetlist((dir / "Bad.setlist").string(), set).ok);
    writeText(dir / "Odd.setlist",
              R"({ "format": "stagehand-setlist", "version": 1, "switching": { "at": "whenever" } })");
    CHECK(!loadSetlist((dir / "Odd.setlist").string(), set).ok);
    writeText(dir / "Trigger.setlist",   // Next needs a note or a controller
              R"({ "format": "stagehand-setlist", "version": 1, "triggers": [ { "action": "next" } ] })");
    CHECK(!loadSetlist((dir / "Trigger.setlist").string(), set).ok);
}

void testPaths(const fs::path& dir) {
    CHECK(setlistPathOf((dir / "Friday").string()) == (dir / "Friday.setlist").string());
    CHECK(setlistPathOf((dir / "Friday.setlist").string()) == (dir / "Friday.setlist").string());
    CHECK(setlistNameOf((dir / "Friday gig.setlist").string()) == "Friday gig");
    CHECK(isSetlistFile((dir / "Old.setlist").string()));
    CHECK(!isSetlistFile((dir / "Racks/So what.rack").string()));
    CHECK(!isSetlistFile((dir / "Missing.setlist").string()));
}

void testLastPosition(const fs::path& dir) {
    const std::string path = (dir / "state/stagehand/last.json").string();
    LastPosition      p;
    CHECK(!loadLastPosition(path, p));
    CHECK(saveLastPosition(path, LastPosition{"/home/me/Friday.setlist", 3}));   // makes the folders
    CHECK(loadLastPosition(path, p) && p.setlist == "/home/me/Friday.setlist" && p.song == 3);
    CHECK(saveLastPosition(path, LastPosition{}));
    CHECK(loadLastPosition(path, p) && p.setlist.empty() && p.song == 0);

    CHECK(defaultLastPositionPath() == appStateDir() + "/last.json");
}

// Where StageHand keeps its own files (Session/AppPaths.h): XDG's folders on
// Linux, Application Support on macOS, from HOME either way.
void testAppPaths() {
    const char*       home     = std::getenv("HOME");
    const std::string realHome = home != nullptr ? home : "";
    setenv("HOME", "/h", 1);
#ifdef __APPLE__
    CHECK(appDataDir() == "/h/Library/Application Support/StageHand");
    CHECK(appStateDir() == "/h/Library/Application Support/StageHand");
#else
    unsetenv("XDG_DATA_HOME");
    unsetenv("XDG_STATE_HOME");
    CHECK(appDataDir() == "/h/.local/share/stagehand" && appStateDir() == "/h/.local/state/stagehand");
    setenv("XDG_DATA_HOME", "/x/data", 1);
    setenv("XDG_STATE_HOME", "/x/state", 1);
    CHECK(appDataDir() == "/x/data/stagehand" && appStateDir() == "/x/state/stagehand");
    CHECK(defaultLastPositionPath() == "/x/state/stagehand/last.json");
    CHECK(defaultWindowStatePath() == "/x/state/stagehand/window.json");
    setenv("XDG_STATE_HOME", "", 1);   // set but empty: as if not set
    CHECK(appStateDir() == "/h/.local/state/stagehand");
    unsetenv("XDG_DATA_HOME");
    unsetenv("XDG_STATE_HOME");
#endif
    setenv("HOME", realHome.c_str(), 1);
}

// The rack window's size, kept between runs, and fitted to the monitor.
void testWindowSize(const fs::path& dir) {
    const std::string path = (dir / "state/stagehand/window.json").string();
    WindowSize        s;
    CHECK(!loadWindowSize(path, s));
    CHECK(saveWindowSize(path, WindowSize{1180.4, 905.6}));
    CHECK(loadWindowSize(path, s) && s == (WindowSize{1180.0, 906.0}));
    writeText(dir / "state/stagehand/window.json", R"({"width": 0, "height": 700})");
    CHECK(!loadWindowSize(path, s) && s == (WindowSize{1180.0, 906.0}));   // unchanged

    CHECK(defaultWindowStatePath() == appStateDir() + "/window.json");

    const WindowSize minimum{930.0, 300.0};
    CHECK(fitWindowSize({1100.0, 900.0}, minimum, {1920.0, 1080.0}) == (WindowSize{1100.0, 900.0}));
    CHECK(fitWindowSize({1100.0, 1200.0}, minimum, {1920.0, 1080.0}) == (WindowSize{1100.0, 1080.0 * 0.92}));
    CHECK(fitWindowSize({1600.0, 900.0}, minimum, {1366.0, 768.0}) == (WindowSize{1366.0 * 0.92, 768.0 * 0.92}));
    CHECK(fitWindowSize({1100.0, 900.0}, minimum, {800.0, 200.0}) == minimum);         // never under the minimum
    CHECK(fitWindowSize({1100.0, 1900.0}, minimum, {}) == (WindowSize{1100.0, 1900.0}));   // the monitor unknown
}

} // namespace

int main() {
    const fs::path dir = scratch();
    testRoundTrip(dir);
    testMissingAndAbsolute(dir);
    testFailures(dir);
    testPaths(dir);
    testLastPosition(dir);
    testWindowSize(dir);
    testAppPaths();
    fs::remove_all(dir);
    return test::finish("SetlistFile");
}
