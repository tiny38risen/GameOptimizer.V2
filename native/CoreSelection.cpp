#include "CoreSelection.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <functional>
#include <iterator>
#include <limits>
#include <optional>
#include <ranges>

namespace gameoptimizer::engine
{
    namespace
    {
        struct Candidate
        {
            std::size_t Position = 0;
            std::size_t BestLp = 0;
            const CoreMeasurement* Core = nullptr;
            double CoreInterruptDpc = 0.0;
        };

        [[nodiscard]] KAFFINITY LpMask(std::uint32_t index) noexcept
        {
            return KAFFINITY{ 1 } << index;
        }

        [[nodiscard]] std::uint32_t L3Of(const Candidate& candidate) noexcept
        {
            return candidate.Core->Core.L3Index;
        }

        template <class Projection>
        void KeepNearBest(std::vector<Candidate>& pool, Projection projection,
            double tolerance)
        {
            if (pool.empty()) return;
            const double best = std::ranges::max(
                pool | std::views::transform(projection));
            std::erase_if(pool, [&](const Candidate& candidate)
            {
                return std::invoke(projection, candidate) < best - tolerance;
            });
        }

        [[nodiscard]] const Candidate& LowestCoreIndex(
            const std::vector<Candidate>& pool)
        {
            return *std::ranges::min_element(pool, {},
                [](const Candidate& candidate) { return candidate.Core->Core.Index; });
        }

        [[nodiscard]] std::optional<Candidate> MakeCandidate(
            std::span<const CoreMeasurement> cores, std::size_t position,
            KAFFINITY usable, double interruptDpcTolerance)
        {
            const CoreMeasurement& core = cores[position];
            std::optional<std::size_t> bestLp;
            double coreInterruptDpc = 0.0;

            for (std::size_t lpPosition = 0; lpPosition < core.Lps.size(); ++lpPosition)
            {
                const LpMeasurement& lp = core.Lps[lpPosition];
                if (!std::isfinite(lp.InterruptDpcPercent)) return std::nullopt;
                coreInterruptDpc += lp.InterruptDpcPercent;
                if ((usable & LpMask(lp.Index)) == 0) continue;

                // Only switch to a clearly quieter LP; ties keep the lower LP number.
                if (!bestLp || lp.InterruptDpcPercent <
                    core.Lps[*bestLp].InterruptDpcPercent - interruptDpcTolerance)
                    bestLp = lpPosition;
            }
            if (!bestLp) return std::nullopt;

            return Candidate{ position, *bestLp, &core, coreInterruptDpc };
        }
    }

    std::vector<CoreMeasurement> BuildMeasurements(
        std::span<const PhysicalCore> topology, const CounterSample& counters,
        std::span<const SpeedSamples> speeds)
    {
        if (speeds.size() != topology.size()) return {};

        const double fastest = speeds.empty() ? 0.0 : std::ranges::max(
            speeds | std::views::transform([](const SpeedSamples& sample)
            {
                return sample.Best;
            }));
        const auto valueAt = [](const std::vector<double>& values, std::size_t index)
        {
            return index < values.size() ? values[index]
                : std::numeric_limits<double>::quiet_NaN();
        };

        std::vector<CoreMeasurement> result;
        result.reserve(topology.size());
        for (std::size_t position = 0; position < topology.size(); ++position)
        {
            const PhysicalCore& core = topology[position];
            const SpeedSamples& speed = speeds[position];
            CoreMeasurement measurement{ .Core = core,
                .RelativeSpeedPercent = fastest > 0.0 ? speed.Best / fastest * 100.0 : 0.0 };

            for (KAFFINITY bits = core.LogicalMask; bits != 0; bits &= bits - 1)
            {
                const auto lp = static_cast<std::uint32_t>(std::countr_zero(bits));
                measurement.Lps.push_back({ .Index = lp,
                    .InterruptDpcPercent = valueAt(counters.InterruptTime, lp) +
                        valueAt(counters.DpcTime, lp) });
            }
            result.push_back(std::move(measurement));
        }
        return result;
    }

    Result<CoreSelection> SelectCores(std::span<const CoreMeasurement> cores,
        KAFFINITY gameMask, KAFFINITY systemMask, const SelectionConfig& config)
    {
        if (cores.empty() || gameMask == 0 || systemMask == 0 ||
            !std::isfinite(config.SpeedTolerancePercent) || config.SpeedTolerancePercent < 0.0 ||
            !std::isfinite(config.InterruptDpcTolerancePercent) ||
            config.InterruptDpcTolerancePercent < 0.0)
            return FailText(L"코어 선정 입력값이 올바르지 않습니다.");

        const KAFFINITY usable = gameMask & systemMask;
        std::vector<Candidate> candidates;
        for (std::size_t position = 0; position < cores.size(); ++position)
        {
            if (!std::isfinite(cores[position].RelativeSpeedPercent) ||
                cores[position].RelativeSpeedPercent <= 0.0)
                continue;
            if (auto candidate = MakeCandidate(cores, position, usable,
                config.InterruptDpcTolerancePercent))
                candidates.push_back(*candidate);
        }

        // Keep only L3 groups containing at least two game-usable physical cores.
        std::vector<Candidate> paired;
        std::ranges::copy_if(candidates, std::back_inserter(paired),
            [&](const Candidate& candidate)
            {
                return std::ranges::count(candidates, L3Of(candidate), L3Of) >= 2;
            });
        if (paired.empty())
            return FailText(L"같은 L3를 공유하는 물리 코어 후보가 두 곳 미만입니다.");

        // Dominant: efficiency class, speed, summed core interrupt/DPC, core index.
        std::vector<Candidate> dominantPool = paired;
        if (std::ranges::all_of(dominantPool, [](const Candidate& candidate)
            { return candidate.Core->Core.EfficiencyClass.has_value(); }))
            KeepNearBest(dominantPool, [](const Candidate& candidate)
            {
                return static_cast<double>(*candidate.Core->Core.EfficiencyClass);
            }, 0.0);
        KeepNearBest(dominantPool, [](const Candidate& candidate)
        {
            return candidate.Core->RelativeSpeedPercent;
        }, config.SpeedTolerancePercent);
        KeepNearBest(dominantPool, [](const Candidate& candidate)
        {
            return -candidate.CoreInterruptDpc;
        }, config.InterruptDpcTolerancePercent);
        const Candidate dominant = LowestCoreIndex(dominantPool);

        // Worker: another physical core in the same L3, then total core interrupt/DPC, core index.
        std::vector<Candidate> workers;
        std::ranges::copy_if(paired, std::back_inserter(workers),
            [&](const Candidate& candidate)
            {
                return candidate.Position != dominant.Position &&
                    L3Of(candidate) == L3Of(dominant);
            });
        if (workers.empty())
            return FailText(L"지배 코어와 같은 L3의 보조 코어가 없습니다.");
        KeepNearBest(workers, [](const Candidate& candidate)
        {
            return -candidate.CoreInterruptDpc;
        }, config.InterruptDpcTolerancePercent);
        const Candidate worker = LowestCoreIndex(workers);

        const KAFFINITY workerLp = LpMask(worker.Core->Lps[worker.BestLp].Index);
        const KAFFINITY reserved =
            (dominant.Core->Core.LogicalMask & systemMask) | workerLp;
        if ((systemMask & ~reserved) == 0)
            return FailText(L"게임 전용 CPU 외에 다른 프로그램이 사용할 논리 프로세서가 없습니다.");

        return CoreSelection{
            .Dominant = { dominant.Position, dominant.BestLp },
            .Worker = { worker.Position, worker.BestLp },
            .Reserved = reserved };
    }
}
