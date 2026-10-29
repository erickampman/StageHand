// Set package tests (docs/documents.md): a new package's round trip, saving in
// place (edited racks only, unused racks gone, the rig kept), Save As to a new
// package, failures, loading with a missing rack, rack names, and making a
// set from an older setlist or a lone rack. Files go in a scratch folder
// under /tmp, which is HOME too, so the default rig is the scratch one.

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "Check.h"
#include "Session/RigFile.h"
#include "Session/SetFile.h"
#include "Session/SetlistFile.h"

using namespace stagehand;
namespace fs = std::filesystem;

namespace {

fs::path scratch() {
    const fs::path dir = fs::temp_directory_path() / ("stagehand-setfile-" + std::to_string(getpid()));
    fs::create_directories(dir);
    return fs::canonical(dir);
}

void writeText(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path) << text;
}

Rack rackAt(double tempo) {
    Rack rack;
    rack.name     = "R";
    rack.tempoBpm = tempo;
    return rack;
}

double tempoOf(const std::string& bundle) {
    Rack rack;
    return loadRack(bundle, rack).ok ? rack.tempoBpm : -1.0;
}

void testRoundTrip(const fs::path& dir) {
    const std::string gig = setPathOf((dir / "Gig").string());
    CHECK(gig == (dir / "Gig.setlist").string() && setNameOf(gig) == "Gig");
    Set set;
    set.songs              = {{"A", ""}, {"B", "B (fast)"}, {"A", "A (reprise)"}};
    set.switching.oldRack  = SwitchSettings::OldRack::Cut;
    Rig rig;
    rig.masterDb = -3.0f;
    CHECK(saveSet(gig, set, &rig, {{"A", rackAt(90.0)}, {"B", rackAt(100.0)}}).ok);
    CHECK(isSetPackage(gig) && !isSetPackage((dir / "Nothing").string()));

    Set loaded;
    const RackFileResult result = loadSet(gig, loaded);
    CHECK(result.ok && result.warnings.empty() && loaded == set);
    CHECK(tempoOf(setRackPath(gig, "A")) == 90.0 && tempoOf(setRackPath(gig, "B")) == 100.0);
    Rig back;
    CHECK(loadRig(setRigPath(gig), back).ok && back.masterDb == -3.0f);
    CHECK(!fs::exists(gig + ".tmp"));
}

void testInPlace(const fs::path& dir) {
    const std::string gig = (dir / "Gig.setlist").string();
    // Only B edited, A's songs removed: B written, A gone, the rig kept.
    Set set;
    set.songs = {{"B", ""}};
    CHECK(saveSet(gig, set, nullptr, {{"B", rackAt(110.0)}}).ok);
    CHECK(tempoOf(setRackPath(gig, "B")) == 110.0 && !fs::exists(setRackPath(gig, "A")));
    Rig rig;
    CHECK(loadRig(setRigPath(gig), rig).ok && rig.masterDb == -3.0f);
    // Nothing edited: the racks on disk stay as they are.
    set.songs.push_back({"B", "again"});
    CHECK(saveSet(gig, set, nullptr, {}).ok && tempoOf(setRackPath(gig, "B")) == 110.0);
    Set loaded;
    CHECK(loadSet(gig, loaded).ok && loaded == set);
}

void testSaveAs(const fs::path& dir) {
    const std::string gig  = (dir / "Gig.setlist").string();
    const std::string copy = (dir / "Copy.setlist").string();
    Set set;
    CHECK(loadSet(gig, set).ok);
    set.songs.push_back({"C", ""});   // a new rack, only in memory
    CHECK(saveSet(copy, set, nullptr, {{"C", rackAt(70.0)}}, gig).ok);
    CHECK(tempoOf(setRackPath(copy, "B")) == 110.0 && tempoOf(setRackPath(copy, "C")) == 70.0);
    CHECK(!fs::exists(setRackPath(gig, "C")));   // the original is untouched
    Rig rig;
    CHECK(loadRig(setRigPath(copy), rig).ok && rig.masterDb == -3.0f);   // its rig copied
    // Saved over an existing set: replaced whole.
    Set one;
    one.songs = {{"C", ""}};
    CHECK(saveSet(gig, one, nullptr, {}, copy).ok);
    CHECK(!fs::exists(setRackPath(gig, "B")) && tempoOf(setRackPath(gig, "C")) == 70.0);
    // A new set from nothing: an empty rig.
    const std::string fresh = (dir / "Fresh.setlist").string();
    CHECK(saveSet(fresh, one, nullptr, {{"C", rackAt(80.0)}}).ok);
    CHECK(loadRig(setRigPath(fresh), rig).ok && rig.masterDb == 0.0f);
}

void testFailures(const fs::path& dir) {
    const std::string gig = (dir / "Gig.setlist").string();
    Set               none;
    CHECK(!saveSet(gig, none, nullptr, {}).ok);   // no songs
    Set bad;
    bad.songs = {{"../Escape", ""}};
    CHECK(!saveSet(gig, bad, nullptr, {{"../Escape", rackAt(1.0)}}).ok);
    bad.songs = {{".hidden", ""}};
    CHECK(!saveSet(gig, bad, nullptr, {{".hidden", rackAt(1.0)}}).ok);
    Set missing;
    missing.songs = {{"Nowhere", ""}};
    CHECK(!saveSet(gig, missing, nullptr, {}).ok);
    CHECK(tempoOf(setRackPath(gig, "C")) == 70.0);   // unchanged by any of those
    // Something that isn't a set is never replaced.
    writeText(dir / "Notes.setlist", "mine");
    Set one;
    one.songs = {{"C", ""}};
    CHECK(!saveSet((dir / "Notes.setlist").string(), one, nullptr, {}, gig).ok);
    std::ifstream in(dir / "Notes.setlist");
    std::string   text;
    in >> text;
    CHECK(text == "mine");

    // Reading: a song whose rack is missing is left out, with a warning.
    writeText(dir / "Hand.setlist/setlist.json",
              R"({"format": "stagehand-set", "version": 1, "songs": [{"rack": "Gone"}, {"rack": "Here"}]})");
    CHECK(saveRack(setRackPath((dir / "Hand.setlist").string(), "Here"), rackAt(60.0)).ok);
    Set hand;
    const RackFileResult result = loadSet((dir / "Hand.setlist").string(), hand);
    CHECK(result.ok && result.warnings.size() == 1 && hand.songs.size() == 1 && hand.songs[0].rack == "Here");
    writeText(dir / "Hand.setlist/setlist.json", R"({"format": "stagehand-set", "version": 2})");
    CHECK(!loadSet((dir / "Hand.setlist").string(), hand).ok);
    writeText(dir / "Hand.setlist/setlist.json", R"({"format": "stagehand-setlist", "version": 1})");
    CHECK(!loadSet((dir / "Hand.setlist").string(), hand).ok);
    writeText(dir / "Hand.setlist/setlist.json", R"({"format": "stagehand-set", "version": 1, "songs": [{"rack": "a/b"}]})");
    CHECK(!loadSet((dir / "Hand.setlist").string(), hand).ok);
}

void testNames() {
    CHECK(validRackName("So what") && !validRackName("") && !validRackName("a/b") && !validRackName(".x"));
    CHECK(freeRackName("So what", {}) == "So what");
    CHECK(freeRackName("So what", {"So what", "So what 2"}) == "So what 3");
}

// An older setlist: racks elsewhere, one of them twice, two with the same
// name in different folders, one missing, and a rig of its own.
void testConvertSetlist(const fs::path& dir) {
    CHECK(saveRack((dir / "old/Racks/X.rack").string(), rackAt(91.0)).ok);
    CHECK(saveRack((dir / "other/X.rack").string(), rackAt(92.0)).ok);
    Rig stage;
    stage.masterDb = -6.0f;
    CHECK(saveRig((dir / "old/Stage.rig").string(), stage).ok);
    Setlist old;
    old.songs = {{(dir / "old/Racks/X.rack").string(), ""},
                 {(dir / "other/X.rack").string(), ""},
                 {(dir / "old/Racks/X.rack").string(), "X (reprise)"},
                 {(dir / "old/Racks/Missing.rack").string(), ""}};
    old.rig                = (dir / "old/Stage.rig").string();
    old.switching.when     = SwitchSettings::When::Now;
    const std::string file = (dir / "old/Friday.setlist").string();
    CHECK(saveSetlist(file, old).ok);

    LoadedSet            converted;
    const RackFileResult result = convertSetlist(file, converted);
    CHECK(result.ok && result.warnings.size() == 1);   // the missing rack
    CHECK(converted.racks.size() == 2 && converted.racks.at("X").tempoBpm == 91.0 &&
          converted.racks.at("X 2").tempoBpm == 92.0 && converted.racks.at("X 2").name == "X 2");
    CHECK(converted.set.songs.size() == 3);
    CHECK(converted.set.songs[0] == (SetSong{"X", ""}) && converted.set.songs[1] == (SetSong{"X 2", "X"}) &&
          converted.set.songs[2] == (SetSong{"X", "X (reprise)"}));
    CHECK(converted.set.switching.when == SwitchSettings::When::Now && converted.rig.masterDb == -6.0f);

    // Saved, it's a set of its own; the old setlist and racks are untouched.
    const std::string set = (dir / "Friday.setlist").string();
    CHECK(saveSet(set, converted.set, &converted.rig, converted.racks).ok);
    CHECK(tempoOf(setRackPath(set, "X 2")) == 92.0 && fs::exists(dir / "old/Racks/X.rack") && fs::exists(file));

    Setlist gone;
    gone.songs = {{(dir / "old/Racks/Missing.rack").string(), ""}};
    CHECK(saveSetlist((dir / "old/Gone.setlist").string(), gone).ok);
    CHECK(!convertSetlist((dir / "old/Gone.setlist").string(), converted).ok);   // nothing left
}

void testSetFromRack(const fs::path& dir) {
    Rig defaults;
    defaults.masterDb = -1.0f;
    CHECK(saveRig(defaultRigPath(), defaults).ok);   // in the scratch HOME
    LoadedSet one;
    CHECK(setFromRack((dir / "other/X.rack").string(), one).ok);
    CHECK(one.set.songs.size() == 1 && one.set.songs[0] == (SetSong{"X", ""}) && one.racks.at("X").tempoBpm == 92.0);
    CHECK(one.rig.masterDb == -1.0f);
    CHECK(!setFromRack((dir / "Nothing.rack").string(), one).ok);
}

} // namespace

int main() {
    const fs::path    dir      = scratch();
    const char*       home     = std::getenv("HOME");
    const std::string realHome = home != nullptr ? home : "";
    setenv("HOME", dir.c_str(), 1);
    testRoundTrip(dir);
    testInPlace(dir);
    testSaveAs(dir);
    testFailures(dir);
    testNames();
    testConvertSetlist(dir);
    testSetFromRack(dir);
    setenv("HOME", realHome.c_str(), 1);
    fs::remove_all(dir);
    return test::finish("SetFile");
}
