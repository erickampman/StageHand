#pragma once

// Helpers shared by the Session's files (racks, rigs, setlists): "~" in paths,
// state blobs, plugin formats, and writing safely. Internal to
// stagehand-session.

#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "Core/MidiSourceRef.h"
#include "uw/plugin/PluginDescriptor.h"
#include "Core/UwNames.h"

namespace stagehand::bundle {

namespace fs = std::filesystem;

// A file's structural problem: caught by the loader, which reports it.
[[noreturn]] inline void fail(const std::string& message) {
    throw std::runtime_error(message);
}

// A MIDI source as files write it: "all", "none", "device:<id>", "virtual:<name>".
inline MidiSourceRef parseSource(const std::string& text) {
    MidiSourceRef ref;
    if (text == "all") return ref;
    if (text == "none") {
        ref.kind = MidiSourceRef::Kind::None;
        return ref;
    }
    if (text.rfind("device:", 0) == 0 && text.size() > 7) {
        ref.kind = MidiSourceRef::Kind::Device;
        ref.name = text.substr(7);
        return ref;
    }
    if (text.rfind("virtual:", 0) == 0 && text.size() > 8) {
        ref.kind = MidiSourceRef::Kind::Virtual;
        ref.name = text.substr(8);
        return ref;
    }
    if (text.rfind("sequence:", 0) == 0 && text.size() > 9) {
        ref.kind = MidiSourceRef::Kind::Sequence;
        ref.name = text.substr(9);
        return ref;
    }
    fail("midi.source '" + text + "' should be \"all\", \"none\", \"device:<id>\" or \"virtual:<name>\"");
}

inline std::string sourceText(const MidiSourceRef& ref) {
    switch (ref.kind) {
        case MidiSourceRef::Kind::All:     return "all";
        case MidiSourceRef::Kind::Device:  return "device:" + ref.name;
        case MidiSourceRef::Kind::Virtual: return "virtual:" + ref.name;
        case MidiSourceRef::Kind::Sequence: return "sequence:" + ref.name;   // never saved (converted on load)
        case MidiSourceRef::Kind::None:    return "none";
    }
    return "all";
}

inline std::string homeDir() {
    const char* home = std::getenv("HOME");
    return home != nullptr ? home : "";
}

inline std::string expandHome(const std::string& path) {
    if (path == "~" || path.rfind("~/", 0) == 0) return homeDir() + path.substr(1);
    return path;
}

inline std::string abbreviateHome(const std::string& path) {
    const std::string home = homeDir();
    if (!home.empty() && path.rfind(home + "/", 0) == 0) return "~" + path.substr(home.size());
    return path;
}

// "X.rack/" -> "X.rack", "~" expanded.
inline fs::path bundleDirectory(const std::string& bundlePath) {
    fs::path bundle = fs::path(expandHome(bundlePath)).lexically_normal();
    if (!bundle.has_filename()) bundle = bundle.parent_path();
    return bundle;
}

inline bool readBytes(const fs::path& path, std::vector<std::byte>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    in.seekg(0, std::ios::beg);
    out.resize(static_cast<std::size_t>(size));
    in.read(reinterpret_cast<char*>(out.data()), size);
    return static_cast<bool>(in);
}

inline bool writeBytes(const fs::path& path, const std::vector<std::byte>& data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(out);
}

inline PluginFormat parseFormat(const std::string& text) {
    if (text == "clap") return PluginFormat::Clap;
    if (text == "vst3") return PluginFormat::Vst3;
    if (text == "auv2") return PluginFormat::Auv2;
    fail("unknown plugin format '" + text + "' (expected \"clap\")");
}

inline const char* formatName(PluginFormat format) {
    switch (format) {
        case PluginFormat::Clap: return "clap";
        case PluginFormat::Vst3: return "vst3";
        case PluginFormat::Auv2: return "auv2";
    }
    return "clap";
}

// Replace the bundle `bundle` with the complete one at `temp`: until the last
// rename the old bundle is untouched (or, briefly, renamed to "<bundle>.old"),
// so a crash never leaves a half-written bundle. Throws on failure.
inline void swapInBundle(const fs::path& temp, const fs::path& bundle) {
    const fs::path old = bundle.string() + ".old";
    fs::remove_all(old);
    if (fs::exists(bundle)) fs::rename(bundle, old);
    fs::rename(temp, bundle);
    fs::remove_all(old);
}

// Write a small file safely: a temporary file renamed over the old one.
inline bool writeTextAtomically(const fs::path& path, const std::string& text, std::string& error) {
    const fs::path  temp = path.string() + ".tmp";
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    {
        std::ofstream out(temp, std::ios::trunc);
        out << text;
        if (!out) {
            error = "cannot write " + temp.string();
            return false;
        }
    }
    fs::rename(temp, path, ec);
    if (ec) {
        error = "cannot replace " + path.string() + ": " + ec.message();
        fs::remove(temp, ec);
        return false;
    }
    return true;
}

} // namespace stagehand::bundle
