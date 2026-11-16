#pragma once

#include "CoreSpeedProbe.h"
#include "CpuTopology.h"
#include "EngineResult.h"
#include "ProcessorCounters.h"
#include "TuningConfig.h"

#include <cstddef>
#include <span>
#include <vector>

namespace gameoptimizer::engine
{
    struct LpMeasurement
    {
        std::uint32_t Index = 0;
        double InterruptDpcPercent = 0.0;
    };

    struct CoreMeasurement
    {
        PhysicalCore Core;
        double RelativeSpeedPercent = 0.0;
        std::vector<LpMeasurement> Lps;
    };

    struct LpChoice
    {
        std::size_t Core = 0;
        std::size_t Lp = 0;
    };

    struct CoreSelection
    {
        LpChoice Dominant;
        LpChoice Worker;
        KAFFINITY Reserved = 0;
    };

    struct SelectionConfig
    {
        double SpeedTolerancePercent = 0.0;
        double InterruptDpcTolerancePercent = 0.0;
    };

    inline constexpr SelectionConfig DefaultSelectionConfig{
        .SpeedTolerancePercent = tuning::SpeedTolerancePercent,
        .InterruptDpcTolerancePercent = tuning::InterruptDpcTolerancePercent };

    [[nodiscard]] std::vector<CoreMeasurement> BuildMeasurements(
        std::span<const PhysicalCore> topology, const CounterSample& counters,
        std::span<const SpeedSamples> speeds);

    [[nodiscard]] Result<CoreSelection> SelectCores(
        std::span<const CoreMeasurement> cores, KAFFINITY gameMask,
        KAFFINITY systemMask, const SelectionConfig& config);
}
