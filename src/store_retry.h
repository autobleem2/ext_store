//
// What the Store does when a download fails: is it the network (wait and continue from the .part) or the
// download itself (fail as before), and how long and how often to wait. No GUI, no threads, the clock is a
// parameter: tested in tests/test_store_retry.cpp.
//
#pragma once

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

// A failure of the network: no address, no connection, a timeout, a stall, a dropped or short transfer.
// `command` is the platform's download command - abfetch's exit codes (src/tools/abfetch/fetch.h in the
// launcher) are not curl's: abfetch 2 = network error, 6 = incomplete; curl (and anything else) 6 = no host,
// 7 = no connect, 18 = partial file, 28 = timeout, 52 = empty reply, 55/56 = send/recv failure.
// abfetch's 3 (TLS), 4 (HTTP status), 5 (cannot write) and 7 (too many redirects) are not the network's.
bool isNetworkFailure(int status, const std::string &command);

struct RetryPolicy {
    using Ms = std::chrono::milliseconds;
    std::vector<Ms> delays{Ms(5000), Ms(15000), Ms(30000), Ms(60000)}; // then the last one again
    Ms giveUp{30 * 60 * 1000}; // no network this long (without a byte more) and the item fails
};

// One item's outage: onFailure() at each network failure says how long to wait, or that it is over.
class NetworkRetry {
public:
    using Clock = std::chrono::steady_clock;
    struct Decision {
        bool giveUp = false;
        RetryPolicy::Ms delay{0};
    };

    explicit NetworkRetry(RetryPolicy policy = RetryPolicy()) : policy(std::move(policy)) {}

    // a network failure at `now`; `progressed` = the download got bytes since the last failure (the outage
    // starts afresh, the waits begin again at the first)
    Decision onFailure(Clock::time_point now, bool progressed);
    // the outage has lasted the policy's limit (checked while waiting for the network to come back)
    bool expired(Clock::time_point now) const;
    int attempts() const { return attempt; }

private:
    RetryPolicy policy;
    bool inOutage = false;
    Clock::time_point since;
    int attempt = 0;
};
