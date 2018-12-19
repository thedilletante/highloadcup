#pragma once
#include <time.h>
#include <string>
namespace HLC {
struct TExecTime {
    TExecTime(std::string timerName)
        : StartTime(std::clock())
        , TimerName(std::move(timerName)) {
    }

    ~TExecTime() {
        printf("Time taken for %s: %.2fs\n", TimerName.c_str(), (double) (std::clock() - StartTime) / CLOCKS_PER_SEC);
    }

    const clock_t StartTime;
    const std::string TimerName;
};
}
#define EXEC_TIME(x) const auto& some_var = HLC::TExecTime(x)