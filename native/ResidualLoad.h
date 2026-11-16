#pragma once

#include "EngineResult.h"
#include "ProcessAffinityController.h"
#include "ProcessorCounters.h"

#include <atomic>
#include <optional>

namespace gameoptimizer::engine
{
    struct ResidualLoad
    {
        double DominantThreadPercent = 0.0;
        double DominantLpResidualPercent = 0.0;
        std::optional<double> SiblingPercent;
        CounterSample Counters;
    };

    [[nodiscard]] Result<ResidualLoad> MeasureResidualLoad(const CpuPlan& plan,
        const ThreadIdentity& dominantThread, HANDLE gameProcess,
        const std::atomic<bool>& stop);
}
