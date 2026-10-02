//
// SpeedMeter and the speed / time-left text of the Store's bubble: units and rounding, the time format, the
// smoothing, its resets, an unknown total.
//
#include "doctest/doctest.h"

#include "../src/store_speed.h"

#include <cstdlib>

using namespace std;
using Clock = SpeedMeter::Clock;

namespace {
const Clock::time_point T0 = Clock::time_point() + chrono::hours(1);

Clock::time_point at(int ms) {
    return T0 + chrono::milliseconds(ms);
}

// a steady download of `rate` bytes/s sampled every 33 ms (a frame) for `ms`
uint64_t feed(SpeedMeter &m, uint64_t rate, int fromMs, int toMs, uint64_t base = 0) {
    uint64_t done = base;
    for (int t = fromMs; t <= toMs; t += 33) {
        done = base + rate * static_cast<uint64_t>(t - fromMs) / 1000;
        m.sample(done, at(t));
    }
    return done;
}
} // namespace

TEST_CASE("formatRate: units and rounding") {
    CHECK(formatRate(0) == "0 B/s");
    CHECK(formatRate(350) == "350 B/s");
    CHECK(formatRate(1023) == "1023 B/s");
    CHECK(formatRate(1024) == "1.0 KB/s");
    CHECK(formatRate(1.4 * 1024) == "1.4 KB/s");
    CHECK(formatRate(9.94 * 1024) == "9.9 KB/s");
    CHECK(formatRate(9.96 * 1024) == "10 KB/s");
    CHECK(formatRate(350 * 1024) == "350 KB/s");
    CHECK(formatRate(1023.4 * 1024) == "1023 KB/s");
    CHECK(formatRate(1023.6 * 1024) == "1.0 MB/s"); // rounds to 1024 KB: the next unit
    CHECK(formatRate(1.4 * 1024 * 1024) == "1.4 MB/s");
    CHECK(formatRate(12.3 * 1024 * 1024) == "12 MB/s");
    CHECK(formatRate(2.5 * 1024 * 1024 * 1024) == "2.5 GB/s");
    CHECK(formatRate(-5) == "0 B/s");
}

TEST_CASE("formatDuration: m:ss, h:mm:ss, hidden when absurd") {
    CHECK(formatDuration(0) == "0:00");
    CHECK(formatDuration(42) == "0:42");
    CHECK(formatDuration(60) == "1:00");
    CHECK(formatDuration(12 * 60 + 5) == "12:05");
    CHECK(formatDuration(3599) == "59:59");
    CHECK(formatDuration(3600) == "1:00:00");
    CHECK(formatDuration(3600 + 2 * 60 + 10) == "1:02:10");
    CHECK(formatDuration(99L * 3600) == "99:00:00");
    CHECK(formatDuration(99L * 3600 + 1) == "");
    CHECK(formatDuration(-1) == "");
}

TEST_CASE("SpeedMeter: nothing until about a second of data") {
    SpeedMeter m;
    CHECK_FALSE(m.ready());
    m.sample(0, at(0));
    feed(m, 1000000, 0, 900);
    CHECK_FALSE(m.ready());
    CHECK(formatSpeedEta(m, 900000, 10000000) == "");
    feed(m, 1000000, 0, 1100);
    CHECK(m.ready());
}

TEST_CASE("SpeedMeter: a steady download reads its rate and time left") {
    SpeedMeter m;
    const uint64_t done = feed(m, 1048576, 0, 4000); // 1 MB/s
    REQUIRE(m.ready());
    CHECK(m.bytesPerSecond() == doctest::Approx(1048576.0).epsilon(0.02));
    const uint64_t total = 61 * 1048576ULL;
    CHECK(labs(m.secondsLeft(done, total) - 57) <= 2);
    CHECK(formatSpeedEta(m, done, total).find("1.0 MB/s \xC2\xB7 ") == 0);
}

TEST_CASE("SpeedMeter: smoothed - a sudden stall or burst moves the number gradually") {
    SpeedMeter m;
    uint64_t done = feed(m, 1000000, 0, 3000);
    const double steady = m.bytesPerSecond();
    // one second of nothing: the rate falls, but is far from 0
    for (int t = 3033; t <= 4000; t += 33)
        m.sample(done, at(t));
    CHECK(m.bytesPerSecond() < steady);
    CHECK(m.bytesPerSecond() > steady * 0.5);
}

TEST_CASE("SpeedMeter: frames closer than the minimal step do not make noise") {
    SpeedMeter m;
    m.sample(0, at(0));
    m.sample(500000, at(10)); // too soon: ignored, nothing to read
    m.sample(1000000, at(1000));
    CHECK(m.ready());
    CHECK(m.bytesPerSecond() == doctest::Approx(1000000.0).epsilon(0.01));
}

TEST_CASE("SpeedMeter: reset, and done going backwards, start over") {
    SpeedMeter m;
    uint64_t done = feed(m, 1000000, 0, 3000);
    REQUIRE(m.ready());
    m.reset();
    CHECK_FALSE(m.ready());
    CHECK(m.bytesPerSecond() == 0.0);
    CHECK(formatSpeedEta(m, done, done * 2) == "");
    done = feed(m, 1000000, 4000, 7000);
    REQUIRE(m.ready());
    m.sample(1000, at(7100)); // the file started again
    CHECK_FALSE(m.ready());
}

TEST_CASE("SpeedMeter: an unknown total, a finished one, a crawl") {
    SpeedMeter m;
    const uint64_t done = feed(m, 1048576, 0, 3000);
    CHECK(m.secondsLeft(done, 0) == -1);
    CHECK(m.secondsLeft(done, done - 1) == -1);
    CHECK(formatSpeedEta(m, done, 0) == "1.0 MB/s"); // the speed alone
    CHECK(formatSpeedEta(m, done, done + 524288) == "1.0 MB/s \xC2\xB7 0:01");

    SpeedMeter slow; // 1 byte/s: the time left is absurd, so only the speed shows
    slow.sample(0, at(0));
    slow.sample(1, at(1000));
    slow.sample(2, at(2000));
    REQUIRE(slow.ready());
    CHECK(formatSpeedEta(slow, 2, 1000000000ULL) == "1 B/s");
}
