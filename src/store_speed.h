//
// The speed and the time left of the download in flight, for the launcher's bubble. No GUI, no strings to
// translate (numbers and units only): tested in tests/test_store_speed.cpp.
//
#pragma once

#include <chrono>
#include <cstdint>
#include <string>

// SpeedMeter: fed the bytes done once a frame, it keeps a smoothed rate (an exponential moving average with a
// time constant of ~2.5 s over steps of at least 250 ms, so a frame's jitter and a chunked download do not
// make the number jump). Nothing is reported before ~1 s of data. reset() on anything that breaks the
// series: another item or state, a pause/resume, a stop of the download.
class SpeedMeter {
public:
    using Clock = std::chrono::steady_clock;

    static constexpr double TimeConstantSeconds = 2.5;
    static constexpr double MinStepSeconds = 0.25;
    static constexpr double WarmUpSeconds = 1.0;

    void reset();
    // `done` bytes at `now`; done going backwards (a restart, a new file) starts over from there
    void sample(uint64_t done, Clock::time_point now);

    bool ready() const { return hasRate && warm; }
    double bytesPerSecond() const { return ready() ? rate : 0.0; }
    // seconds left for `total` bytes (0 = unknown): -1 when not known - no total, no speed, or done past it
    long secondsLeft(uint64_t done, uint64_t total) const;

private:
    bool started = false, hasRate = false, warm = false;
    Clock::time_point startTime, anchorTime;
    uint64_t anchorDone = 0;
    double rate = 0.0;
};

// "350 B/s", "1.4 KB/s", "350 KB/s", "1.4 MB/s", "12 MB/s", "1.1 GB/s": 1024-based, one decimal under 10
std::string formatRate(double bytesPerSecond);
// "0:42", "12:05", "1:02:10"; "" for a negative or an absurd (over 99 h) number of seconds
std::string formatDuration(long seconds);
// "1.4 MB/s · 0:42", just "1.4 MB/s" when the time left is unknown, "" when there is no speed yet
std::string formatSpeedEta(const SpeedMeter &meter, uint64_t done, uint64_t total);
