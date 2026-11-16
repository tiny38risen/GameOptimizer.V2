#pragma once

#include "EngineResult.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace gameoptimizer::engine
{
    struct PhysicalCore
    {
        std::uint32_t Index = 0;
        KAFFINITY LogicalMask = 0;
        std::uint32_t L3Index = 0;
        std::uint64_t L3SizeBytes = 0;
        std::optional<std::uint8_t> EfficiencyClass;
    };

    [[nodiscard]] Result<std::vector<PhysicalCore>> ReadCpuTopology();
}
