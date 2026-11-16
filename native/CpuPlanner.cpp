#include "CpuPlanner.h"

#include "CoreSelection.h"
#include "CoreSpeedProbe.h"
#include "CpuTopology.h"
#include "DiagnosticLog.h"
#include "ProcessorCounters.h"

#include <TlHelp32.h>

#include <algorithm>
#include <chrono>
#include <ranges>
#include <span>
#include <stop_token>
#include <thread>

namespace gameoptimizer::engine
{
    namespace
    {
        using namespace std::chrono_literals;
        constexpr auto CounterInterval = 500ms;
        constexpr auto MeasurementBudget = 9500ms;

        Result<void> LogSpecialThreadAffinities(HANDLE process, KAFFINITY processMask,
            DiagnosticLog& log)
        {
            const DWORD processId = GetProcessId(process);
            UniqueHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0));
            if (!snapshot.IsValid()) return Fail(L"게임 스레드 목록을 읽지 못했습니다");

            THREADENTRY32 entry{};
            entry.dwSize = sizeof(entry);
            BOOL hasEntry = Thread32First(snapshot.Get(), &entry);
            if (!hasEntry && GetLastError() != ERROR_NO_MORE_FILES)
                return Fail(L"게임 스레드 목록을 읽지 못했습니다");

            while (hasEntry)
            {
                if (entry.th32OwnerProcessID == processId)
                {
                    UniqueHandle thread(OpenThread(
                        THREAD_QUERY_INFORMATION | SYNCHRONIZE, FALSE, entry.th32ThreadID));
                    if (!thread.IsValid())
                    {
                        if (GetLastError() != ERROR_INVALID_PARAMETER)
                            return Fail(L"게임 스레드 affinity를 저장하지 못했습니다");
                    }
                    else if (GetProcessIdOfThread(thread.Get()) == processId)
                    {
                        GROUP_AFFINITY affinity{};
                        if (!GetThreadGroupAffinity(thread.Get(), &affinity))
                        {
                            if (WaitForSingleObject(thread.Get(), 0) != WAIT_OBJECT_0)
                                return Fail(L"게임 스레드 affinity를 읽지 못했습니다");
                        }
                        else if (affinity.Group != 0 || affinity.Mask != processMask)
                        {
                            log.Write(L"special thread pid={} tid={} group={} mask={:#x}",
                                processId, entry.th32ThreadID, affinity.Group, affinity.Mask);
                        }
                    }
                }

                entry.dwSize = sizeof(entry);
                hasEntry = Thread32Next(snapshot.Get(), &entry);
                if (!hasEntry && GetLastError() != ERROR_NO_MORE_FILES)
                    return Fail(L"게임 스레드 목록을 끝까지 읽지 못했습니다");
            }
            return {};
        }

        GROUP_AFFINITY SingleProcessor(std::uint32_t index) noexcept
        {
            GROUP_AFFINITY result{};
            result.Group = 0;
            result.Mask = KAFFINITY{ 1 } << index;
            return result;
        }

        CpuCoreCandidate ToCandidate(const CoreMeasurement& core,
            const LpMeasurement& lp)
        {
            return CpuCoreCandidate{ core.Core.Index, lp.Index,
                SingleProcessor(lp.Index), core.Core.L3Index,
                core.RelativeSpeedPercent, lp.InterruptDpcPercent,
                core.Core.EfficiencyClass };
        }

        CpuPlan ToCpuPlan(const CoreSelection& selection,
            std::span<const CoreMeasurement> cores)
        {
            const auto& dominant = cores[selection.Dominant.Core];
            const auto& worker = cores[selection.Worker.Core];
            CpuPlan plan{};
            plan.DominantCore = ToCandidate(dominant, dominant.Lps[selection.Dominant.Lp]);
            plan.WorkerCore = ToCandidate(worker, worker.Lps[selection.Worker.Lp]);
            plan.Dominant = plan.DominantCore.LogicalProcessor;
            plan.Workers = plan.WorkerCore.LogicalProcessor;
            plan.Game.Group = 0;
            plan.Game.Mask = plan.Dominant.Mask | plan.Workers.Mask;
            plan.Reserved.Group = 0;
            plan.Reserved.Mask = selection.Reserved;
            return plan;
        }

        void LogSpeedSamples(DiagnosticLog& log,
            std::span<const PhysicalCore> topology,
            std::span<const SpeedSamples> speeds)
        {
            for (const auto& [core, samples] : std::views::zip(topology, speeds))
            {
                log.Write(L"speed core={} best={:.2f}", core.Index, samples.Best);
                for (const ProbeSample& sample : samples.Values)
                    log.Write(L"speed core={} lp={} value={:.2f}",
                        core.Index, sample.Lp, sample.IterationsPerMs);
            }
        }

        void LogMeasurements(DiagnosticLog& log,
            std::span<const CoreMeasurement> cores, const CounterSample& counters)
        {
            const auto read = [](const std::vector<double>& values, std::uint32_t index)
            {
                return index < values.size() ? values[index] : -1.0;
            };
            for (const auto& core : cores)
            {
                log.Write(L"core={} lpMask={:#x} l3={} l3bytes={} speed={:.2f} eff={}",
                    core.Core.Index, core.Core.LogicalMask, core.Core.L3Index, core.Core.L3SizeBytes,
                    core.RelativeSpeedPercent,
                    core.Core.EfficiencyClass
                        ? static_cast<int>(*core.Core.EfficiencyClass) : -1);
                for (const auto& lp : core.Lps)
                    log.Write(L"counter lp={} processor={:.2f} interrupt={:.2f} dpc={:.2f}",
                        lp.Index, read(counters.ProcessorTime, lp.Index),
                        read(counters.InterruptTime, lp.Index),
                        read(counters.DpcTime, lp.Index));
            }
        }

        void LogPlan(DiagnosticLog& log, const CpuPlan& plan)
        {
            log.Write(L"dominant core={} lp={} speed={:.2f}% intdpc={:.2f}% "
                L"worker core={} lp={} speed={:.2f}% intdpc={:.2f}% game={:#x} reserved={:#x}",
                plan.DominantCore.PhysicalCoreIndex, plan.DominantCore.LogicalProcessorIndex,
                plan.DominantCore.RelativeSpeedPercent, plan.DominantCore.InterruptDpcPercent,
                plan.WorkerCore.PhysicalCoreIndex, plan.WorkerCore.LogicalProcessorIndex,
                plan.WorkerCore.RelativeSpeedPercent, plan.WorkerCore.InterruptDpcPercent,
                plan.Game.Mask, plan.Reserved.Mask);
        }
    }

    Result<CpuPlan> SelectCpuPlan(HANDLE gameProcess,
        const std::atomic<bool>& stopRequested, DiagnosticLog& log)
    {
        if (stopRequested.load()) return Cancelled();
        KAFFINITY gameMask = 0;
        KAFFINITY systemMask = 0;
        if (!GetProcessAffinityMask(gameProcess, &gameMask, &systemMask))
            return Fail(L"CPU 목록을 읽지 못했습니다");
        log.Write(L"game affinity original={:#x} system={:#x}", gameMask, systemMask);

        if (WaitForSingleObject(gameProcess, 0) != WAIT_TIMEOUT)
            return std::unexpected(EngineError{
                L"측정 시작 전에 대상 게임이 종료되었습니다.", ERROR_PROCESS_ABORTED });
        auto specialThreads = LogSpecialThreadAffinities(gameProcess, gameMask, log);
        if (!specialThreads) return std::unexpected(specialThreads.error());

        auto topology = ReadCpuTopology();
        if (!topology) return std::unexpected(topology.error());
        if (stopRequested.load()) return Cancelled();

        KAFFINITY optimizerMask = 0;
        KAFFINITY optimizerSystemMask = 0;
        if (!GetProcessAffinityMask(GetCurrentProcess(), &optimizerMask, &optimizerSystemMask))
            return Fail(L"측정 프로그램의 CPU 범위를 읽지 못했습니다");
        const KAFFINITY probeMask =
            gameMask & systemMask & optimizerMask & optimizerSystemMask;
        if (probeMask == 0)
            return FailText(L"게임과 측정 프로그램이 함께 사용할 CPU가 없습니다.");

        const auto measureStarted = Clock::now();
        const auto measureDeadline = measureStarted + MeasurementBudget;
        std::stop_source measureStop;
        Result<std::vector<SpeedSamples>> speeds =
            FailText(L"코어 속도 측정이 완료되지 않았습니다.");
        std::jthread probe([&]
        {
            try
            {
                speeds = ProbeCoreSpeeds(*topology, probeMask, measureDeadline,
                    measureStop.get_token(), stopRequested);
            }
            catch (...)
            {
                speeds = FailText(L"코어 속도 측정 중 오류가 발생했습니다.");
            }
            if (!speeds) measureStop.request_stop();
        });

        auto counters = ReadProcessorCountersUntil(CounterInterval, measureDeadline,
            measureStop.get_token(), stopRequested, gameProcess);
        if (!counters) measureStop.request_stop();
        probe.join();

        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - measureStarted).count();
        log.Write(L"measure elapsed={}ms", elapsed);

        if (!counters && counters.error().Code != ERROR_CANCELLED)
            return std::unexpected(counters.error());
        if (!speeds && speeds.error().Code != ERROR_CANCELLED)
            return std::unexpected(speeds.error());
        if (stopRequested.load())
        {
            log.Write(L"measure cancelled");
            return Cancelled();
        }
        if (!counters) return std::unexpected(counters.error());
        if (!speeds) return std::unexpected(speeds.error());

        LogSpeedSamples(log, *topology, *speeds);
        const auto cores = BuildMeasurements(*topology, *counters, *speeds);
        LogMeasurements(log, cores, *counters);
        log.Write(L"selection config speedTolerance={:.2f} intdpcTolerance={:.2f}",
            DefaultSelectionConfig.SpeedTolerancePercent,
            DefaultSelectionConfig.InterruptDpcTolerancePercent);

        auto selection = SelectCores(cores, gameMask, systemMask, DefaultSelectionConfig);
        if (!selection)
        {
            log.Write(L"selection failed: {}", selection.error().Message);
            return std::unexpected(selection.error());
        }

        CpuPlan plan = ToCpuPlan(*selection, cores);
        LogPlan(log, plan);
        return plan;
    }
}
