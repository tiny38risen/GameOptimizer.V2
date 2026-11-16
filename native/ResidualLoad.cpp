#include "ResidualLoad.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <utility>

namespace gameoptimizer::engine
{
    namespace
    {
        using namespace std::chrono_literals;

        struct ThreadTimes
        {
            std::uint64_t Creation = 0;
            std::uint64_t Busy = 0;
        };

        struct ThreadWindow
        {
            HANDLE Thread = nullptr;
            std::optional<ThreadTimes> Start;
            std::optional<ThreadTimes> End;
            std::chrono::steady_clock::time_point StartAt{};
            std::chrono::steady_clock::time_point EndAt{};
        };

        [[nodiscard]] std::optional<ThreadTimes> ReadThreadTimes(HANDLE thread) noexcept
        {
            FILETIME creation{}, exit{}, kernel{}, user{};
            if (!GetThreadTimes(thread, &creation, &exit, &kernel, &user))
                return std::nullopt;
            return ThreadTimes{ FileTimeValue(creation),
                FileTimeValue(kernel) + FileTimeValue(user) };
        }

        [[nodiscard]] bool ProcessAlive(HANDLE process) noexcept
        {
            return process && WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
        }

        void CaptureThreadWindow(void* context, bool starting) noexcept
        {
            auto& window = *static_cast<ThreadWindow*>(context);
            const auto beforeRead = std::chrono::steady_clock::now();
            const auto times = ReadThreadTimes(window.Thread);
            const auto afterRead = std::chrono::steady_clock::now();
            const auto midpoint = beforeRead + (afterRead - beforeRead) / 2;
            if (starting)
            {
                window.Start = times;
                window.StartAt = midpoint;
            }
            else
            {
                window.End = times;
                window.EndAt = midpoint;
            }
        }
    }

    Result<ResidualLoad> MeasureResidualLoad(const CpuPlan& plan,
        const ThreadIdentity& dominantThread, HANDLE gameProcess,
        const std::atomic<bool>& stop)
    {
        if (!gameProcess || GetProcessId(gameProcess) != dominantThread.ProcessId ||
            plan.Dominant.Mask == 0)
            return FailText(L"잔여 부하 측정 대상이 올바르지 않습니다.");
        if (stop.load()) return Cancelled();
        if (!ProcessAlive(gameProcess))
            return std::unexpected(EngineError{
                L"잔여 부하 측정 전에 대상 게임이 종료되었습니다.", ERROR_PROCESS_ABORTED });

        const UniqueHandle thread(OpenThread(
            THREAD_QUERY_INFORMATION | SYNCHRONIZE, FALSE, dominantThread.ThreadId));
        if (!thread.IsValid()) return Fail(L"지배 스레드를 열지 못했습니다");

        GROUP_AFFINITY affinity{};
        if (!GetThreadGroupAffinity(thread.Get(), &affinity))
            return Fail(L"지배 스레드의 CPU 배치를 읽지 못했습니다");
        if (GetThreadId(thread.Get()) != dominantThread.ThreadId ||
            GetProcessIdOfThread(thread.Get()) != dominantThread.ProcessId ||
            affinity.Group != plan.Dominant.Group || affinity.Mask != plan.Dominant.Mask)
            return FailText(L"지배 스레드 신원 또는 CPU 배치를 확인하지 못했습니다.");

        const auto initial = ReadThreadTimes(thread.Get());
        if (!initial || initial->Creation != dominantThread.CreationFileTime)
            return FailText(L"지배 스레드 신원을 확인하지 못했습니다.");

        ThreadWindow window{ .Thread = thread.Get() };
        auto counters = ReadProcessorCounters(500ms, 6, stop, gameProcess,
            CaptureThreadWindow, &window);
        if (!counters) return std::unexpected(counters.error());
        if (stop.load()) return Cancelled();

        if (!window.Start || !window.End)
            return FailText(L"지배 스레드 측정 구간을 읽지 못했습니다.");
        if (WaitForSingleObject(thread.Get(), 0) != WAIT_TIMEOUT || !ProcessAlive(gameProcess))
            return std::unexpected(EngineError{
                L"잔여 부하 측정 중 게임 또는 지배 스레드가 종료되었습니다.",
                ERROR_PROCESS_ABORTED });
        if (window.Start->Creation != dominantThread.CreationFileTime ||
            window.End->Creation != dominantThread.CreationFileTime ||
            window.End->Busy < window.Start->Busy)
            return FailText(L"지배 스레드의 측정 구간이 올바르지 않습니다.");
        GROUP_AFFINITY finalAffinity{};
        if (!GetThreadGroupAffinity(thread.Get(), &finalAffinity) ||
            finalAffinity.Group != plan.Dominant.Group ||
            finalAffinity.Mask != plan.Dominant.Mask)
            return FailText(L"측정 중 지배 스레드의 CPU 배치가 바뀌었습니다.");

        const double elapsed100ns =
            std::chrono::duration<double>(window.EndAt - window.StartAt).count() * 1e7;
        if (elapsed100ns <= 0.0)
            return FailText(L"잔여 부하 측정 시간이 올바르지 않습니다.");
        const double threadPercent =
            static_cast<double>(window.End->Busy - window.Start->Busy) /
            elapsed100ns * 100.0;

        const auto busyOf = [&](KAFFINITY mask) -> std::optional<double>
        {
            if (mask == 0) return std::nullopt;
            const auto lp = static_cast<std::size_t>(std::countr_zero(mask));
            if (lp >= counters->ProcessorTime.size()) return std::nullopt;
            return counters->ProcessorTime[lp];
        };

        const auto dominantBusy = busyOf(plan.Dominant.Mask);
        if (!dominantBusy) return FailText(L"지배 LP 측정값이 없습니다.");

        ResidualLoad result{ .DominantThreadPercent = threadPercent,
            .DominantLpResidualPercent = std::max(0.0, *dominantBusy - threadPercent) };

        const KAFFINITY siblings =
            plan.Reserved.Mask & ~plan.Workers.Mask & ~plan.Dominant.Mask;
        for (KAFFINITY bits = siblings; bits != 0; bits &= bits - 1)
            if (const auto busy = busyOf(bits))
                result.SiblingPercent = std::max(result.SiblingPercent.value_or(0.0), *busy);

        result.Counters = std::move(*counters);
        return result;
    }
}
