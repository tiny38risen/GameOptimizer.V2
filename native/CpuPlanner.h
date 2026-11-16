#pragma once

#include "EngineResult.h"
#include "ProcessAffinityController.h"

#include <atomic>

namespace gameoptimizer::engine
{
    class DiagnosticLog;

    [[nodiscard]] Result<CpuPlan> SelectCpuPlan(HANDLE gameProcess,
        const std::atomic<bool>& stopRequested, DiagnosticLog& log);
}
