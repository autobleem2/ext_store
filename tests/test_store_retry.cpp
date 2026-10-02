//
// The network-failure rule: which exit codes are the network's (abfetch's and curl's differ), and the waits
// between tries, the new start after progress, and the give-up after the policy's limit - on a fake clock.
//
#include "doctest/doctest.h"

#include "../src/store_retry.h"

using namespace std;
using Clock = NetworkRetry::Clock;

namespace {
const Clock::time_point T0 = Clock::time_point() + chrono::hours(1);
Clock::time_point at(int seconds) {
    return T0 + chrono::seconds(seconds);
}
} // namespace

TEST_CASE("isNetworkFailure: curl's codes") {
    const string curl = "curl -sfL -C - -o %o %u";
    for (int code : {6, 7, 18, 28, 52, 55, 56})
        CHECK(isNetworkFailure(code, curl));
    for (int code : {0, 1, 2, 3, 22, 23, 33, 35, 60, 63, 130, -2})
        CHECK_FALSE(isNetworkFailure(code, curl)); // 22 = HTTP 404, 23 = cannot write
}

TEST_CASE("isNetworkFailure: abfetch's own codes are not curl's") {
    const string abfetch = "\"%r/abfetch\" --continue --connect-timeout 20 --stall-timeout 60 -o \"%o\" \"%u\"";
    CHECK(isNetworkFailure(2, abfetch));       // NetworkError
    CHECK(isNetworkFailure(6, abfetch));       // Incomplete
    CHECK_FALSE(isNetworkFailure(7, abfetch)); // too many redirects (curl's 7 is a failed connect)
    CHECK_FALSE(isNetworkFailure(3, abfetch)); // TLS
    CHECK_FALSE(isNetworkFailure(4, abfetch)); // HTTP status
    CHECK_FALSE(isNetworkFailure(5, abfetch)); // cannot write
    CHECK_FALSE(isNetworkFailure(0, abfetch));
    CHECK_FALSE(isNetworkFailure(28, abfetch));
}

TEST_CASE("NetworkRetry: 5 s, 15 s, 30 s, then 60 s every time") {
    NetworkRetry retry;
    int t = 0;
    const int expected[] = {5, 15, 30, 60, 60, 60};
    for (int seconds : expected) {
        const NetworkRetry::Decision d = retry.onFailure(at(t), false);
        REQUIRE_FALSE(d.giveUp);
        CHECK(d.delay == chrono::seconds(seconds));
        t += seconds;
    }
    CHECK(retry.attempts() == 6);
}

TEST_CASE("NetworkRetry: gives up after the limit of no network, and says so while waiting too") {
    NetworkRetry retry;
    CHECK_FALSE(retry.expired(at(0))); // no outage yet
    CHECK_FALSE(retry.onFailure(at(0), false).giveUp);
    CHECK_FALSE(retry.expired(at(1799)));
    CHECK(retry.expired(at(1800)));
    CHECK_FALSE(retry.onFailure(at(1799), false).giveUp);
    CHECK(retry.onFailure(at(1800), false).giveUp);
}

TEST_CASE("NetworkRetry: bytes got since the last failure start the outage afresh") {
    NetworkRetry retry;
    retry.onFailure(at(0), false);
    retry.onFailure(at(5), false);
    retry.onFailure(at(20), false); // 30 s is next
    const NetworkRetry::Decision d = retry.onFailure(at(1700), true);
    CHECK_FALSE(d.giveUp);
    CHECK(d.delay == chrono::seconds(5)); // back to the first wait
    CHECK_FALSE(retry.expired(at(1700 + 1799)));
    CHECK(retry.expired(at(1700 + 1800)));
}

TEST_CASE("NetworkRetry: a policy of its own (short waits, short limit)") {
    RetryPolicy p;
    p.delays = {RetryPolicy::Ms(10), RetryPolicy::Ms(20)};
    p.giveUp = RetryPolicy::Ms(100);
    NetworkRetry retry(p);
    const auto t0 = Clock::time_point() + chrono::hours(1);
    CHECK(retry.onFailure(t0, false).delay == chrono::milliseconds(10));
    CHECK(retry.onFailure(t0 + chrono::milliseconds(10), false).delay == chrono::milliseconds(20));
    CHECK(retry.onFailure(t0 + chrono::milliseconds(30), false).delay == chrono::milliseconds(20));
    CHECK(retry.onFailure(t0 + chrono::milliseconds(100), false).giveUp);
}
