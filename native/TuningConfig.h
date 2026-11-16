#pragma once

#include <chrono>

namespace gameoptimizer::engine::tuning
{
    using Clock = std::chrono::steady_clock;

    inline constexpr auto MeasurementBudget = std::chrono::milliseconds{ 9500 };
    inline constexpr auto MeasurementCounterInterval = std::chrono::milliseconds{ 500 };
    inline constexpr auto ProbeWarmup = std::chrono::milliseconds{ 20 };
    inline constexpr auto ProbeMeasurement = std::chrono::milliseconds{ 80 };
    inline constexpr auto ProbeRest = std::chrono::milliseconds{ 50 };

    inline constexpr double SpeedTolerancePercent = 1.0;
    inline constexpr double InterruptDpcTolerancePercent = 0.5;
    inline constexpr double ResidualWarningPercent = 5.0;

    inline constexpr auto ResidualSampleInterval = std::chrono::milliseconds{ 500 };
    inline constexpr int ResidualSampleCount = 6;
}
