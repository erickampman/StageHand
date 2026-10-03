// The beat as heard (App/BeatNow.h, docs/perform.md decision 14): carried on
// from the engine's last published position by the time since, less the
// output latency; bars, pulses and the count-in's countdown from it.

#include <cmath>

#include "App/BeatNow.h"
#include "Check.h"

using namespace stagehand;

namespace {

constexpr Nanoseconds kSecond = 1'000'000'000;

Engine::Transport running(double beat, double barStart, int bar) {
    Engine::Transport t;   // 120 BPM, 4/4: a beat is half a second
    t.running       = true;
    t.beatPosition  = beat;
    t.barStartBeats = barStart;
    t.barNumber     = bar;
    t.at            = 10 * kSecond;
    return t;
}

void testStopped() {
    Engine::Transport t;
    CHECK(beatNow(t, 11 * kSecond, 0.0, false).state == BeatNow::State::Stopped);
}

void testRunning() {
    // Published at 10 s: beat 4 (bar 2's downbeat, 0-based bar 1).
    const Engine::Transport t = running(4.0, 4.0, 1);
    BeatNow b = beatNow(t, 10 * kSecond, 0.0, false);
    CHECK(b.state == BeatNow::State::Running && b.bar == 2 && b.beat == 1 && b.pulse == 0 && b.downbeat);
    CHECK(std::fabs(b.sincePulse) < 1e-9);
    // 0.6 s later: beat 5.2, the second pulse, 0.1 s into it.
    b = beatNow(t, 10 * kSecond + 600'000'000, 0.0, false);
    CHECK(b.bar == 2 && b.beat == 2 && !b.downbeat && std::fabs(b.sincePulse - 0.1) < 1e-9);
    // Into the next bar, past what was published: bar 3.
    b = beatNow(t, 12 * kSecond + 100'000'000, 0.0, false);
    CHECK(b.bar == 3 && b.beat == 1 && std::fabs(b.sincePulse - 0.1) < 1e-9);
    // 20 ms of output latency: heard 20 ms (0.04 beats) behind.
    b = beatNow(t, 10 * kSecond + 600'000'000, 0.02, false);
    CHECK(b.beat == 2 && std::fabs(b.sincePulse - 0.08) < 1e-9);
    // Just after the published downbeat, but not heard yet: the bar before's last pulse.
    b = beatNow(t, 10 * kSecond + 10'000'000, 0.02, false);
    CHECK(b.bar == 1 && b.beat == 4 && std::fabs(b.sincePulse - 0.49) < 1e-9);
}

void testCompound() {
    Engine::Transport t = running(0.0, 0.0, 0);   // 6/8, counted: two dotted quarters (0.75 s each)
    t.numerator   = 6;
    t.denominator = 8;
    BeatNow b     = beatNow(t, 10 * kSecond + 800'000'000, 0.0, false);
    CHECK(b.pulsesInBar == 2 && b.beat == 2 && std::fabs(b.sincePulse - 0.05) < 1e-9);
    b = beatNow(t, 10 * kSecond + 800'000'000, 0.0, true);   // every note: six eighths (0.25 s)
    CHECK(b.pulsesInBar == 6 && b.beat == 4 && std::fabs(b.sincePulse - 0.05) < 1e-9);
}

void testJustStarted() {
    // Started at the published block; with 30 ms of latency it isn't heard yet.
    const Engine::Transport t = running(0.0, 0.0, 0);
    const BeatNow b = beatNow(t, 10 * kSecond + 10'000'000, 0.03, false);
    CHECK(b.state == BeatNow::State::Running && b.bar == 1 && b.beat == 1 && b.sincePulse < 0.0);
}

void testCountIn() {
    // Half a bar of 4/4: two pulses, "3, 4"; published 0.25 beats in.
    Engine::Transport t;
    t.countIn       = 2;
    t.countInPulses = 2;
    t.countInBeat   = 0.25;
    t.at            = 10 * kSecond;
    BeatNow b       = beatNow(t, 10 * kSecond, 0.0, false);
    CHECK(b.state == BeatNow::State::CountingIn && b.countIn == 2 && b.pulse == 2 && !b.downbeat);
    CHECK(std::fabs(b.sincePulse - 0.125) < 1e-9);
    b = beatNow(t, 10 * kSecond + 400'000'000, 0.0, false);   // 1.05 beats in: "4"
    CHECK(b.countIn == 1 && b.pulse == 3 && std::fabs(b.sincePulse - 0.025) < 1e-9);
    b = beatNow(t, 11 * kSecond, 0.0, false);   // past its end, before the engine says bar 1: still "1"
    CHECK(b.state == BeatNow::State::CountingIn && b.countIn == 1);
    // Two bars: the first pulse is a downbeat.
    t.countIn = t.countInPulses = 8;
    t.countInBeat               = 0.0;
    b                           = beatNow(t, 10 * kSecond, 0.0, false);
    CHECK(b.countIn == 8 && b.pulse == 0 && b.downbeat);
}

} // namespace

int main() {
    testStopped();
    testRunning();
    testCompound();
    testJustStarted();
    testCountIn();
    return test::finish("BeatNow");
}
