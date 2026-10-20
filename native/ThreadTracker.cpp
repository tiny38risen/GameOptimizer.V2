#include "ThreadTracker.h"

#include <TlHelp32.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace gameoptimizer::engine
{
    namespace
    {
        constexpr auto ConfirmationTime = std::chrono::seconds(4);
        constexpr int MinimumWindows = 4;
        constexpr double MinimumDominantCpuShare = 0.80;
    }

    class ThreadCandidateTracker::Impl
    {
    public:
        CandidateObservation Observe(DWORD processId) // 게임 스레드별 CPU 시간 비교·지배 후보 안정성 확인
        {
            UniqueHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0));
            if (!snapshot.IsValid())
            {
                const DWORD error = GetLastError();
                Reset();
                previousCpu_.clear();
                return { false, false, {}, ErrorMessage(L"스레드 목록 조회 실패", error) };
            }

            std::unordered_map<ThreadIdentity, std::uint64_t, ThreadIdentityHash> currentCpu;
            bool complete = true;
            THREADENTRY32 entry{};
            entry.dwSize = sizeof(entry);
            BOOL hasEntry = Thread32First(snapshot.Get(), &entry);
            if (!hasEntry && GetLastError() != ERROR_NO_MORE_FILES)
                complete = false;

            while (hasEntry)
            {
                if (entry.th32OwnerProcessID == processId)
                {
                    UniqueHandle thread(OpenThread(
                        THREAD_QUERY_INFORMATION | SYNCHRONIZE, FALSE, entry.th32ThreadID));
                    DWORD owner = 0;
                    FILETIME creation{}, exit{}, kernel{}, user{};
                    if (!thread.IsValid() ||
                        (owner = GetProcessIdOfThread(thread.Get())) != processId ||
                        !GetThreadTimes(thread.Get(), &creation, &exit, &kernel, &user))
                    {
                        complete = false;
                    }
                    else
                    {
                        ThreadIdentity identity{ processId, entry.th32ThreadID, FileTimeValue(creation) };
                        currentCpu[identity] = FileTimeValue(kernel) + FileTimeValue(user);
                    }
                }

                entry.dwSize = sizeof(entry);
                hasEntry = Thread32Next(snapshot.Get(), &entry);
                if (!hasEntry && GetLastError() != ERROR_NO_MORE_FILES)
                    complete = false;
            }

            if (!complete)
            {
                previousCpu_.clear();
                Reset();
                return { false, false, {}, L"일부 스레드 신원을 확인할 수 없어 후보를 다시 관찰합니다." };
            }

            struct Delta
            {
                ThreadIdentity Identity;
                std::uint64_t CpuTime;
            };
            std::vector<Delta> deltas;
            deltas.reserve(currentCpu.size());
            std::uint64_t totalCpuDelta = 0;
            for (const auto& [identity, cpuTime] : currentCpu)
            {
                const auto previous = previousCpu_.find(identity);
                const std::uint64_t delta = previous != previousCpu_.end() && cpuTime >= previous->second
                    ? cpuTime - previous->second : 0;
                deltas.push_back({ identity, delta });
                totalCpuDelta += delta;
            }

            previousCpu_ = std::move(currentCpu);
            if (deltas.empty())
            {
                Reset();
                return { false, false, {}, L"CPU 사용 표본이 아직 없습니다." };
            }

            const auto selected = std::max_element(deltas.begin(), deltas.end(),
                [](const Delta& left, const Delta& right) { return left.CpuTime < right.CpuTime; });
            if (selected->CpuTime == 0)
            {
                Reset();
                return { false, false, {}, L"CPU 사용 표본이 아직 없습니다." };
            }

            const double cpuShare = totalCpuDelta == 0
                ? 0.0
                : static_cast<double>(selected->CpuTime) / static_cast<double>(totalCpuDelta);
            if (cpuShare < MinimumDominantCpuShare)
            {
                Reset();
                return { false, false, {}, L"지배 스레드 기준을 확인 중입니다." };
            }

            const auto now = std::chrono::steady_clock::now();
            if (!currentCandidate_ || !(*currentCandidate_ == selected->Identity))
            {
                currentCandidate_ = selected->Identity;
                candidateSince_ = now;
                candidateWindows_ = 1;
            }
            else
            {
                ++candidateWindows_;
            }

            const bool confirmed = candidateWindows_ >= MinimumWindows &&
                now - candidateSince_ >= ConfirmationTime;
            return { true, confirmed, selected->Identity,
                confirmed ? L"CPU 지배 스레드 확인" : L"지배 스레드 안정성 확인 중" };
        }

    private:
        void Reset() noexcept // 지배 스레드 후보 관찰 상태 초기화
        {
            currentCandidate_.reset();
            candidateSince_ = {};
            candidateWindows_ = 0;
        }

        std::unordered_map<ThreadIdentity, std::uint64_t, ThreadIdentityHash> previousCpu_;
        std::optional<ThreadIdentity> currentCandidate_;
        std::chrono::steady_clock::time_point candidateSince_{};
        int candidateWindows_ = 0;
    };

    ThreadCandidateTracker::ThreadCandidateTracker() // CPU 표본 추적 상태 생성
        : impl_(std::make_unique<Impl>())
    {
    }

    ThreadCandidateTracker::~ThreadCandidateTracker() = default; // 내부 추적 상태 해제

    CandidateObservation ThreadCandidateTracker::Observe(DWORD processId) // 지배 스레드 후보·확인 상태 반환
    {
        return impl_->Observe(processId);
    }
}
