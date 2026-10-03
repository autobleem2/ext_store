#include "store_speed.h"

#include <cmath>

using namespace std;

//*******************************
// SpeedMeter::reset / sample / secondsLeft
//*******************************
void SpeedMeter::reset() {
    *this = SpeedMeter();
}

void SpeedMeter::sample(uint64_t done, Clock::time_point now) {
    if (!started || done < anchorDone || now < anchorTime) {
        reset();
        started = true;
        startTime = anchorTime = now;
        anchorDone = done;
        return;
    }
    const double dt = chrono::duration<double>(now - anchorTime).count();
    if (dt < MinStepSeconds)
        return;
    const double instant = static_cast<double>(done - anchorDone) / dt;
    if (!hasRate) {
        rate = instant;
        hasRate = true;
    } else {
        rate += (1.0 - exp(-dt / TimeConstantSeconds)) * (instant - rate);
    }
    anchorTime = now;
    anchorDone = done;
    if (chrono::duration<double>(now - startTime).count() >= WarmUpSeconds)
        warm = true;
}

long SpeedMeter::secondsLeft(uint64_t done, uint64_t total) const {
    if (!ready() || total == 0 || done > total || rate < 1.0)
        return -1;
    const double left = ceil(static_cast<double>(total - done) / rate);
    return left > 1e9 ? -1 : static_cast<long>(left);
}

//*******************************
// formatRate
//*******************************
string formatRate(double bytesPerSecond) {
    static const char *const Units[] = {"B/s", "KB/s", "MB/s", "GB/s"};
    double value = bytesPerSecond < 0 ? 0 : bytesPerSecond;
    int unit = 0;
    // a value that rounds to 1024 of its unit belongs to the next one
    while (unit < 3 && llround(value) >= 1024) {
        value /= 1024.0;
        unit++;
    }
    string number;
    if (unit > 0 && value < 9.95) {
        number = to_string(llround(value * 10));
        number.insert(number.size() - 1, "."); // value is at least 1.0 here: two digits at least
    } else {
        number = to_string(llround(value));
    }
    return number + " " + Units[unit];
}

//*******************************
// formatDuration
//*******************************
string formatDuration(long seconds) {
    if (seconds < 0 || seconds > 99L * 3600)
        return "";
    const long h = seconds / 3600, m = seconds / 60 % 60, s = seconds % 60;
    string out;
    if (h > 0)
        out = to_string(h) + ":" + (m < 10 ? "0" : "") + to_string(m);
    else
        out = to_string(m);
    return out + ":" + (s < 10 ? "0" : "") + to_string(s);
}

//*******************************
// formatSpeedEta
//*******************************
string formatSpeedEta(const SpeedMeter &meter, uint64_t done, uint64_t total) {
    if (!meter.ready())
        return "";
    string text = formatRate(meter.bytesPerSecond());
    const string eta = formatDuration(meter.secondsLeft(done, total));
    if (!eta.empty())
        text += " \xC2\xB7 " + eta;
    return text;
}
