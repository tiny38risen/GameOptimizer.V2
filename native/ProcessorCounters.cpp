#include "ProcessorCounters.h"

#include <Pdh.h>
#include <PdhMsg.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

namespace gameoptimizer::engine
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        struct PdhQueryCloser
        {
            void operator()(PDH_HQUERY query) const noexcept
            {
                if (query) PdhCloseQuery(query);
            }
        };
        using UniquePdhQuery = std::unique_ptr<std::remove_pointer_t<PDH_HQUERY>, PdhQueryCloser>;

        constexpr std::array CounterPaths{
            L"\\Processor Information(*)\\% Processor Time",
            L"\\Processor Information(*)\\% Interrupt Time",
            L"\\Processor Information(*)\\% DPC Time" };

        [[nodiscard]] std::unexpected<EngineError> PdhFail(
            std::wstring_view message, PDH_STATUS status)
        {
            return std::unexpected(EngineError{
                std::format(L"{} (PDH 0x{:08X})", message, static_cast<std::uint32_t>(status)),
                static_cast<DWORD>(status) });
        }

        [[nodiscard]] bool StopRequested(std::stop_token token,
            const std::atomic<bool>& stop) noexcept
        {
            return token.stop_requested() || stop.load();
        }

        bool TargetAlive(HANDLE process) noexcept
        {
            return !process || WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
        }

        [[nodiscard]] Result<void> WaitUntil(Clock::time_point target,
            std::stop_token token, const std::atomic<bool>& stop, HANDLE process)
        {
            while (Clock::now() < target)
            {
                if (StopRequested(token, stop)) return Cancelled();
                if (!TargetAlive(process))
                    return std::unexpected(EngineError{
                        L"측정 중 대상 게임이 종료되었습니다.", ERROR_PROCESS_ABORTED });

                const auto nextCheck = std::min(target, Clock::now() + std::chrono::milliseconds(20));
                std::this_thread::sleep_until(nextCheck);
            }
            if (StopRequested(token, stop)) return Cancelled();
            if (!TargetAlive(process))
                return std::unexpected(EngineError{
                    L"측정 중 대상 게임이 종료되었습니다.", ERROR_PROCESS_ABORTED });
            return {};
        }

        struct Accumulator
        {
            std::vector<double> Total;
            std::vector<unsigned int> Count;
        };

        [[nodiscard]] Result<void> Accumulate(PDH_HCOUNTER counter,
            Accumulator& accumulator)
        {
            DWORD bytes = 0;
            DWORD items = 0;
            PDH_STATUS status = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE,
                &bytes, &items, nullptr);
            if (status != PDH_MORE_DATA || bytes == 0)
                return PdhFail(L"CPU 카운터 목록을 읽지 못했습니다", status);

            std::vector<std::byte> buffer(bytes);
            auto* values = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
            status = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE,
                &bytes, &items, values);
            if (status != ERROR_SUCCESS)
                return PdhFail(L"CPU 카운터 값을 읽지 못했습니다", status);

            for (const auto& item : std::span(values, items))
            {
                if (item.FmtValue.CStatus != PDH_CSTATUS_VALID_DATA &&
                    item.FmtValue.CStatus != PDH_CSTATUS_NEW_DATA)
                    continue;

                unsigned int group = 0;
                unsigned int processor = 0;
                if (swscanf_s(item.szName, L"%u,%u", &group, &processor) != 2 ||
                    group != 0 || processor >= accumulator.Total.size())
                    continue;

                const double value = item.FmtValue.doubleValue;
                if (value < 0.0 || value > 100.0) continue;
                accumulator.Total[processor] += value;
                ++accumulator.Count[processor];
            }
            return {};
        }

        [[nodiscard]] Result<std::vector<double>> Average(const Accumulator& accumulator)
        {
            std::vector<double> result(accumulator.Total.size());
            for (std::size_t lp = 0; lp < result.size(); ++lp)
            {
                if (accumulator.Count[lp] == 0)
                    return FailText(std::format(L"논리 프로세서 {}의 측정값이 비어 있습니다.", lp));
                result[lp] = accumulator.Total[lp] / accumulator.Count[lp];
            }
            return result;
        }

        [[nodiscard]] Result<CounterSample> ReadProcessorCountersImpl(
            std::chrono::milliseconds interval, std::optional<int> sampleLimit,
            Clock::time_point deadline, std::stop_token token,
            const std::atomic<bool>& stop, HANDLE monitoredProcess,
            CounterWindowCallback windowCallback, void* callbackContext)
        {
            const DWORD processorCount = GetActiveProcessorCount(0);
            if (processorCount == 0)
                return Fail(L"논리 프로세서 수를 읽지 못했습니다");
            if (interval <= std::chrono::milliseconds::zero() ||
                (sampleLimit && *sampleLimit <= 0))
                return FailText(L"CPU 측정 간격 또는 표본 수가 올바르지 않습니다.");
            if (StopRequested(token, stop)) return Cancelled();
            if (!TargetAlive(monitoredProcess))
                return std::unexpected(EngineError{
                    L"측정 시작 전에 대상 게임이 종료되었습니다.", ERROR_PROCESS_ABORTED });

            PDH_HQUERY rawQuery = nullptr;
            if (const PDH_STATUS status = PdhOpenQueryW(nullptr, 0, &rawQuery);
                status != ERROR_SUCCESS)
                return PdhFail(L"CPU 카운터 측정을 시작하지 못했습니다", status);
            const UniquePdhQuery query(rawQuery);

            std::array<PDH_HCOUNTER, CounterPaths.size()> counters{};
            for (std::size_t index = 0; index < CounterPaths.size(); ++index)
            {
                if (const PDH_STATUS status = PdhAddEnglishCounterW(
                    query.get(), CounterPaths[index], 0, &counters[index]);
                    status != ERROR_SUCCESS)
                    return PdhFail(L"CPU 카운터를 추가하지 못했습니다", status);
            }

            if (const PDH_STATUS status = PdhCollectQueryData(query.get());
                status != ERROR_SUCCESS)
                return PdhFail(L"CPU 카운터 기준값을 수집하지 못했습니다", status);
            if (windowCallback) windowCallback(callbackContext, true);

            std::array<Accumulator, CounterPaths.size()> accumulators;
            for (Accumulator& accumulator : accumulators)
            {
                accumulator.Total.assign(processorCount, 0.0);
                accumulator.Count.assign(processorCount, 0);
            }

            auto nextSampleAt = Clock::now() + interval;
            int collected = 0;
            while ((!sampleLimit || collected < *sampleLimit) && nextSampleAt <= deadline)
            {
                auto waited = WaitUntil(nextSampleAt, token, stop, monitoredProcess);
                if (!waited) return std::unexpected(waited.error());

                if (const PDH_STATUS status = PdhCollectQueryData(query.get());
                    status != ERROR_SUCCESS)
                    return PdhFail(L"CPU 카운터 표본을 수집하지 못했습니다", status);
                ++collected;
                if (sampleLimit && collected == *sampleLimit && windowCallback)
                    windowCallback(callbackContext, false);

                for (std::size_t index = 0; index < counters.size(); ++index)
                    if (const auto added = Accumulate(counters[index], accumulators[index]); !added)
                        return std::unexpected(added.error());

                nextSampleAt += interval;
            }

            if (sampleLimit && collected != *sampleLimit)
                return FailText(L"CPU 카운터 표본을 모두 수집하지 못했습니다.");
            if (collected == 0)
                return FailText(L"측정 제한 시간 안에 CPU 카운터 표본을 얻지 못했습니다.");

            auto processorTime = Average(accumulators[0]);
            if (!processorTime) return std::unexpected(processorTime.error());
            auto interruptTime = Average(accumulators[1]);
            if (!interruptTime) return std::unexpected(interruptTime.error());
            auto dpcTime = Average(accumulators[2]);
            if (!dpcTime) return std::unexpected(dpcTime.error());

            return CounterSample{ std::move(*processorTime), std::move(*interruptTime),
                std::move(*dpcTime) };
        }
    }

    Result<CounterSample> ReadProcessorCounters(std::chrono::milliseconds interval,
        int samples, const std::atomic<bool>& stop, HANDLE monitoredProcess,
        CounterWindowCallback windowCallback, void* callbackContext)
    {
        return ReadProcessorCountersImpl(interval, samples, Clock::time_point::max(), {},
            stop, monitoredProcess, windowCallback, callbackContext);
    }

    Result<CounterSample> ReadProcessorCountersUntil(std::chrono::milliseconds interval,
        std::chrono::steady_clock::time_point deadline, std::stop_token token,
        const std::atomic<bool>& stop, HANDLE monitoredProcess)
    {
        return ReadProcessorCountersImpl(interval, std::nullopt, deadline, token,
            stop, monitoredProcess, nullptr, nullptr);
    }
}
