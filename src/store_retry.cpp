#include "store_retry.h"

#include <algorithm>

using namespace std;

//*******************************
// isNetworkFailure
//*******************************
bool isNetworkFailure(int status, const string &command) {
    if (command.find("abfetch") != string::npos)
        return status == 2 || status == 6;
    switch (status) {
    case 6:  // could not resolve host
    case 7:  // could not connect
    case 18: // partial file
    case 28: // timeout
    case 52: // empty reply
    case 55: // send failure
    case 56: // receive failure
        return true;
    default:
        return false;
    }
}

//*******************************
// NetworkRetry
//*******************************
NetworkRetry::Decision NetworkRetry::onFailure(Clock::time_point now, bool progressed) {
    if (!inOutage || progressed) {
        inOutage = true;
        since = now;
        attempt = 0;
    }
    Decision d;
    if (expired(now)) {
        d.giveUp = true;
        return d;
    }
    const size_t last = policy.delays.empty() ? 0 : policy.delays.size() - 1;
    d.delay = policy.delays.empty() ? RetryPolicy::Ms(0) : policy.delays[min(static_cast<size_t>(attempt), last)];
    attempt++;
    return d;
}

bool NetworkRetry::expired(Clock::time_point now) const {
    return inOutage && now - since >= policy.giveUp;
}
