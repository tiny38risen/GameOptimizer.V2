#pragma once

#include "EngineResult.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <stop_token>
#include <vector>

namespace gameoptimizer::engine
{
    using CounterWindowCallback = std::function<void(bool)>;

    struct CounterSample
    {
        std::vector<double> ProcessorTime;
        std::vector<double> InterruptTime;
        std::vector<double> DpcTime;
    };

    // Reads group-0 PDH instances without changing the game's affinity. The optional
    // callback brackets the measured counter window for synchronized thread sampling.
    [[nodiscard]] Result<CounterSample> ReadProcessorCounters(
        std::chrono::milliseconds interval, int samples,
        const std::atomic<bool>& stop, HANDLE monitoredProcess = nullptr,
        CounterWindowCallback windowCallback = {});

    [[nodiscard]] Result<CounterSample> ReadProcessorCountersUntil(
        std::chrono::milliseconds interval, std::chrono::steady_clock::time_point deadline,
        std::stop_token token, const std::atomic<bool>& stop,
        HANDLE monitoredProcess = nullptr);
}
