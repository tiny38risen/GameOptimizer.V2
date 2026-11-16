#include "CoreSpeedProbe.h"
#include "TuningConfig.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdint>
#include <ranges>
#include <thread>

namespace gameoptimizer::engine
{
    namespace
    {
        constexpr int BatchSize = 1024;

        [[nodiscard]] bool StopRequested(std::stop_token token,
            const std::atomic<bool>& sessionStop) noexcept
        {
            return token.stop_requested() || sessionStop.load();
        }

        [[nodiscard]] std::uint32_t NthLogicalProcessor(KAFFINITY mask, int n) noexcept
        {
            for (; n > 0; --n) mask &= mask - 1;
            return static_cast<std::uint32_t>(std::countr_zero(mask));
        }

        [[nodiscard]] Result<std::uint64_t> SpinUntil(tuning::Clock::time_point deadline,
            std::uint64_t& state, std::stop_token token,
            const std::atomic<bool>& sessionStop)
        {
            std::uint64_t iterations = 0;
            while (tuning::Clock::now() < deadline)
            {
                for (int index = 0; index < BatchSize; ++index)
                {
                    state ^= state << 13;
                    state ^= state >> 7;
                    state ^= state << 17;
                }
                iterations += BatchSize;

                if (StopRequested(token, sessionStop)) return Cancelled();
            }
            return iterations;
        }

        [[nodiscard]] Result<double> MeasureIterationsPerMs(
            std::atomic<std::uint64_t>& sink, std::stop_token token,
            const std::atomic<bool>& sessionStop)
        {
            std::uint64_t state = 0x9E3779B97F4A7C15ULL;
            auto warmup = SpinUntil(tuning::Clock::now() + tuning::ProbeWarmup,
                state, token, sessionStop);
            if (!warmup) return std::unexpected(warmup.error());

            const auto start = tuning::Clock::now();
            auto measured = SpinUntil(start + tuning::ProbeMeasurement, state, token, sessionStop);
            const auto end = tuning::Clock::now();
            if (!measured) return std::unexpected(measured.error());

            sink.fetch_add(state, std::memory_order_relaxed);
            const double elapsedMs =
                std::chrono::duration<double, std::milli>(end - start).count();
            if (elapsedMs <= 0.0 || *measured == 0)
                return FailText(L"코어 속도 표본이 유효하지 않습니다.");
            return static_cast<double>(*measured) / elapsedMs;
        }
    }

    Result<std::vector<SpeedSamples>> ProbeCoreSpeeds(
        std::span<const PhysicalCore> cores, KAFFINITY probeMask,
        std::chrono::steady_clock::time_point deadline, std::stop_token token,
        const std::atomic<bool>& sessionStop)
    {
        const auto measurable = [probeMask](const PhysicalCore& core)
        {
            return (core.LogicalMask & probeMask) != 0;
        };
        if (std::ranges::none_of(cores, measurable))
            return FailText(L"속도를 측정할 수 있는 코어가 없습니다.");

        std::vector<SpeedSamples> results(cores.size());
        std::atomic<std::uint64_t> sink{ 0 };
        const HANDLE self = GetCurrentThread();
        const std::size_t count = cores.size();

        for (std::size_t slot = 0;
            tuning::Clock::now() + tuning::ProbeWarmup + tuning::ProbeMeasurement <= deadline; ++slot)
        {
            if (StopRequested(token, sessionStop)) return Cancelled();

            const std::size_t round = slot / count;
            const std::size_t position = (round + slot) % count;
            const KAFFINITY available = cores[position].LogicalMask & probeMask;
            if (available == 0) continue;

            const auto logicalProcessorCount =
                static_cast<std::size_t>(std::popcount(available));
            const std::uint32_t lp = NthLogicalProcessor(available,
                static_cast<int>(round % logicalProcessorCount));
            const GROUP_AFFINITY affinity{
                .Mask = KAFFINITY{ 1 } << lp, .Group = 0 };
            if (!SetThreadGroupAffinity(self, &affinity, nullptr))
                return Fail(L"속도 측정 스레드를 코어에 배치하지 못했습니다");

            auto value = MeasureIterationsPerMs(sink, token, sessionStop);
            if (!value) return std::unexpected(value.error());

            results[position].Values.push_back({ .Lp = lp, .IterationsPerMs = *value });
            results[position].Best = std::max(results[position].Best, *value);

            if (round > 0 && tuning::Clock::now() + tuning::ProbeRest <= deadline)
                std::this_thread::sleep_for(tuning::ProbeRest);
        }

        for (std::size_t position = 0; position < cores.size(); ++position)
            if (measurable(cores[position]) && results[position].Values.empty())
                return FailText(L"제한 시간 안에 모든 코어의 속도를 측정하지 못했습니다.");

        return results;
    }
}
