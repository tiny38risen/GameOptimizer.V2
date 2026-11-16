#pragma once

#include "CpuTopology.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <span>
#include <stop_token>
#include <vector>

namespace gameoptimizer::engine
{
    using Clock = std::chrono::steady_clock;

    struct ProbeSample
    {
        std::uint32_t Lp = 0;
        double IterationsPerMs = 0.0;
    };

    struct SpeedSamples
    {
        std::vector<ProbeSample> Values;
        double Best = 0.0;
    };

    [[nodiscard]] Result<std::vector<SpeedSamples>> ProbeCoreSpeeds(
        std::span<const PhysicalCore> cores, KAFFINITY probeMask,
        Clock::time_point deadline, std::stop_token token,
        const std::atomic<bool>& sessionStop);
}
