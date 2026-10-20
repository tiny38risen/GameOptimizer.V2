#include "ThreadPolicy.h"
#include "RecoveryJournal.h"

#include <TlHelp32.h>

#include <algorithm>
#include <iterator>
#include <utility>
#include <vector>

namespace gameoptimizer::engine
{
    class ThreadPolicyController::Impl
    {
        struct PendingRestore
        {
            UniqueHandle Thread;
            ThreadIdentity Identity;
            GROUP_AFFINITY OriginalAffinity{};
            GROUP_AFFINITY AppliedAffinity{};
            int OriginalPriority = THREAD_PRIORITY_NORMAL;
            int AppliedPriority = THREAD_PRIORITY_NORMAL;
            bool NeedsAffinityRestore = false;
            bool NeedsPriorityRestore = false;

            PendingRestore(UniqueHandle&& thread, const ThreadIdentity& identity,
                const GROUP_AFFINITY& originalAffinity, int originalPriority) // 기존 스레드 CPU 배치·우선순위 저장
                : Thread(std::move(thread)), Identity(identity),
                  OriginalAffinity(originalAffinity), AppliedAffinity(originalAffinity),
                  OriginalPriority(originalPriority), AppliedPriority(originalPriority)
            {
            }
        };

        struct ObservedThread
        {
            UniqueHandle Thread;
            ThreadIdentity Identity;
            GROUP_AFFINITY Affinity{};
            int Priority = THREAD_PRIORITY_NORMAL;
        };

    public:
        bool HasPendingRecovery() const noexcept // 게임 스레드 affinity·우선순위 원복 여부 확인
        {
            return std::any_of(pending_.begin(), pending_.end(),
                [](const PendingRestore& item)
                {
                    return item.NeedsAffinityRestore || item.NeedsPriorityRestore;
                });
        }

        const std::wstring& LastError() const noexcept { return lastError_; } // 최근 스레드 정책 오류 조회

        ApplyResult Apply(HANDLE process, DWORD processId,
            std::uint64_t processCreationTime, const std::wstring& processPath,
            const CpuPlan& plan, const ThreadIdentity& dominantThread,
            const std::wstring& journalPath,
            const std::atomic<bool>& stopRequested) // 지배 스레드 CPU 1·나머지 게임 스레드 CPU 2 배정
        {
            if (HasPendingRecovery() && FindPending(dominantThread) == pending_.end())
                return { false, L"새 지배 스레드를 적용하기 전에 이전 설정을 복구해야 합니다." };

            std::wstring error;
            if (!ValidateTarget(process, processId, processCreationTime, processPath, error))
                return { false, error };

            UniqueHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0));
            if (!snapshot.IsValid())
                return { false, ErrorMessage(L"스레드 목록 조회 실패") };

            std::vector<ObservedThread> observed;
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
                        THREAD_QUERY_INFORMATION | THREAD_SET_INFORMATION | SYNCHRONIZE,
                        FALSE, entry.th32ThreadID));
                    if (!thread.IsValid())
                    {
                        if (GetLastError() != ERROR_INVALID_PARAMETER)
                            complete = false;
                    }
                    else
                    {
                        DWORD owner = GetProcessIdOfThread(thread.Get());
                        FILETIME creation{}, exit{}, kernel{}, user{};
                        GROUP_AFFINITY affinity{};
                        int priority = THREAD_PRIORITY_NORMAL;
                        if (owner != processId ||
                            !GetThreadTimes(thread.Get(), &creation, &exit, &kernel, &user) ||
                            !GetThreadGroupAffinity(thread.Get(), &affinity) ||
                            !ReadPriority(thread.Get(), priority))
                        {
                            complete = false;
                        }
                        else
                        {
                            ThreadIdentity identity{ processId, entry.th32ThreadID,
                                FileTimeValue(creation) };
                            observed.push_back({ std::move(thread), identity, affinity, priority });
                        }
                    }
                }

                entry.dwSize = sizeof(entry);
                hasEntry = Thread32Next(snapshot.Get(), &entry);
                if (!hasEntry && GetLastError() != ERROR_NO_MORE_FILES)
                    complete = false;
            }

            if (!complete)
                return { false, L"일부 게임 스레드의 CPU 설정을 읽지 못했습니다." };

            const auto candidate = std::find_if(observed.begin(), observed.end(),
                [&dominantThread](const ObservedThread& item)
                {
                    return item.Identity == dominantThread;
                });
            if (candidate == observed.end())
                return { false, L"적용 직전에 지배 스레드가 종료되거나 바뀌었습니다." };

            for (auto& item : observed)
            {
                if (stopRequested.load())
                    return FailAndAttemptRestore(L"스레드 설정 적용 중 중단 요청을 받았습니다.");

                const bool isDominant = item.Identity == dominantThread;
                const GROUP_AFFINITY desiredAffinity = isDominant ? plan.Dominant : plan.Workers;
                auto pending = FindPending(item.Identity);
                GROUP_AFFINITY originalAffinity = item.Affinity;
                int originalPriority = item.Priority;
                if (pending != pending_.end())
                {
                    originalAffinity = pending->OriginalAffinity;
                    originalPriority = pending->OriginalPriority;
                    if ((!SameAffinity(item.Affinity, pending->AppliedAffinity) &&
                            !SameAffinity(item.Affinity, pending->OriginalAffinity)) ||
                        (item.Priority != pending->AppliedPriority &&
                            item.Priority != pending->OriginalPriority))
                    {
                        return FailAndAttemptRestore(
                            L"게임 스레드 설정이 외부에서 변경되어 안전하게 적용할 수 없습니다.");
                    }
                }

                // 나머지 우선순위 유지·지배 스레드 한 단계 상향
                const int desiredPriority = isDominant
                    ? THREAD_PRIORITY_ABOVE_NORMAL
                    : originalPriority;

                if (pending == pending_.end() &&
                    (!SameAffinity(item.Affinity, desiredAffinity) ||
                     item.Priority != desiredPriority))
                {
                    pending_.emplace_back(std::move(item.Thread), item.Identity,
                        originalAffinity, originalPriority);
                    pending = std::prev(pending_.end());
                }

                HANDLE threadHandle = pending != pending_.end()
                    ? pending->Thread.Get()
                    : item.Thread.Get();

                if (!SameAffinity(item.Affinity, desiredAffinity))
                {
                    if (!ValidateTarget(process, processId, processCreationTime, processPath, error))
                        return FailAndAttemptRestore(error);
                    if (!RecordThreadAffinity(journalPath, processCreationTime,
                        item.Identity, originalAffinity, desiredAffinity, error))
                        return FailAndAttemptRestore(error);
                    if (!SetThreadGroupAffinity(threadHandle, &desiredAffinity, nullptr))
                        return FailAndAttemptRestore(ErrorMessage(L"스레드 CPU 배치 적용 실패"));

                    if (pending != pending_.end())
                    {
                        pending->AppliedAffinity = desiredAffinity;
                        pending->NeedsAffinityRestore = !SameAffinity(
                            pending->OriginalAffinity, desiredAffinity);
                    }
                }
                else if (pending != pending_.end())
                {
                    pending->AppliedAffinity = desiredAffinity;
                    pending->NeedsAffinityRestore = !SameAffinity(
                        pending->OriginalAffinity, desiredAffinity);
                }

                if (item.Priority != desiredPriority)
                {
                    if (!ValidateTarget(process, processId, processCreationTime, processPath, error))
                        return FailAndAttemptRestore(error);
                    if (!RecordThreadPriority(journalPath, processCreationTime,
                        item.Identity, originalPriority, desiredPriority, error))
                        return FailAndAttemptRestore(error);
                    if (!SetThreadPriority(threadHandle, desiredPriority))
                        return FailAndAttemptRestore(ErrorMessage(L"지배 스레드 우선순위 적용 실패"));

                    if (pending != pending_.end())
                    {
                        pending->AppliedPriority = desiredPriority;
                        pending->NeedsPriorityRestore =
                            pending->OriginalPriority != desiredPriority;
                    }
                }
                else if (pending != pending_.end())
                {
                    pending->AppliedPriority = desiredPriority;
                    pending->NeedsPriorityRestore =
                        pending->OriginalPriority != desiredPriority;
                }

                GROUP_AFFINITY verifiedAffinity{};
                int verifiedPriority = THREAD_PRIORITY_NORMAL;
                if (!GetThreadGroupAffinity(threadHandle, &verifiedAffinity) ||
                    !SameAffinity(verifiedAffinity, desiredAffinity) ||
                    !ReadPriority(threadHandle, verifiedPriority) ||
                    verifiedPriority != desiredPriority)
                {
                    return FailAndAttemptRestore(L"게임 스레드 설정 적용 후 읽기 검증에 실패했습니다.");
                }
            }

            lastError_.clear();
            return { true, L"지배 스레드와 나머지 게임 스레드 설정을 확인했습니다." };
        }

        bool TryRestore() // 저장된 게임 스레드 affinity·우선순위 복구
        {
            lastError_.clear();
            auto item = pending_.begin();
            while (item != pending_.end())
            {
                const DWORD wait = WaitForSingleObject(item->Thread.Get(), 0);
                if (wait == WAIT_OBJECT_0)
                {
                    item = pending_.erase(item);
                    continue;
                }
                if (wait != WAIT_TIMEOUT || !MatchesIdentity(item->Thread.Get(), item->Identity))
                {
                    lastError_ = L"복구 대상 스레드의 신원을 확인할 수 없습니다.";
                    ++item;
                    continue;
                }

                bool restored = true;
                if (item->NeedsAffinityRestore)
                {
                    GROUP_AFFINITY current{};
                    if (!GetThreadGroupAffinity(item->Thread.Get(), &current))
                    {
                        lastError_ = L"복구 전 스레드 CPU 배치를 읽지 못했습니다.";
                        restored = false;
                    }
                    else if (SameAffinity(current, item->OriginalAffinity))
                    {
                        item->NeedsAffinityRestore = false;
                    }
                    else if (!SameAffinity(current, item->AppliedAffinity))
                    {
                        lastError_ = L"스레드 CPU 배치가 외부에서 변경되어 안전한 원복을 확인할 수 없습니다.";
                        restored = false;
                    }
                    else if (!SetThreadGroupAffinity(item->Thread.Get(),
                        &item->OriginalAffinity, nullptr) ||
                        !GetThreadGroupAffinity(item->Thread.Get(), &current) ||
                        !SameAffinity(current, item->OriginalAffinity))
                    {
                        lastError_ = L"원래 스레드 CPU 배치를 복구하거나 검증하지 못했습니다.";
                        restored = false;
                    }
                    else
                    {
                        item->NeedsAffinityRestore = false;
                    }
                }

                if (item->NeedsPriorityRestore)
                {
                    int current = THREAD_PRIORITY_NORMAL;
                    if (!ReadPriority(item->Thread.Get(), current))
                    {
                        lastError_ = L"복구 전 스레드 우선순위를 읽지 못했습니다.";
                        restored = false;
                    }
                    else if (current == item->OriginalPriority)
                    {
                        item->NeedsPriorityRestore = false;
                    }
                    else if (current != item->AppliedPriority)
                    {
                        lastError_ = L"스레드 우선순위가 외부에서 변경되어 안전한 원복을 확인할 수 없습니다.";
                        restored = false;
                    }
                    else if (!SetThreadPriority(item->Thread.Get(), item->OriginalPriority) ||
                        GetThreadPriority(item->Thread.Get()) != item->OriginalPriority)
                    {
                        lastError_ = L"원래 스레드 우선순위를 복구하거나 검증하지 못했습니다.";
                        restored = false;
                    }
                    else
                    {
                        item->NeedsPriorityRestore = false;
                    }
                }

                if (restored && !item->NeedsAffinityRestore && !item->NeedsPriorityRestore)
                    item = pending_.erase(item);
                else
                    ++item;
            }

            return pending_.empty();
        }

    private:
        std::vector<PendingRestore>::iterator FindPending(const ThreadIdentity& identity) // 스레드별 저장 원복 정보 조회
        {
            return std::find_if(pending_.begin(), pending_.end(),
                [&identity](const PendingRestore& item) { return item.Identity == identity; });
        }

        static bool SameAffinity(const GROUP_AFFINITY& left,
            const GROUP_AFFINITY& right) noexcept // 그룹·CPU 마스크 비교
        {
            return left.Group == right.Group && left.Mask == right.Mask;
        }

        static bool ReadPriority(HANDLE thread, int& priority) noexcept // GetThreadPriority 결과·오류 상태 확인
        {
            SetLastError(ERROR_SUCCESS);
            priority = GetThreadPriority(thread);
            return priority != THREAD_PRIORITY_ERROR_RETURN || GetLastError() == ERROR_SUCCESS;
        }

        static bool MatchesIdentity(HANDLE thread, const ThreadIdentity& identity) noexcept // 원복 스레드 PID·TID·생성 시각 확인
        {
            if (GetThreadId(thread) != identity.ThreadId ||
                GetProcessIdOfThread(thread) != identity.ProcessId)
                return false;

            FILETIME creation{}, exit{}, kernel{}, user{};
            return GetThreadTimes(thread, &creation, &exit, &kernel, &user) &&
                FileTimeValue(creation) == identity.CreationFileTime;
        }

        ApplyResult FailAndAttemptRestore(const std::wstring& message) // 적용 실패 시 변경된 스레드 설정 즉시 복구
        {
            if (TryRestore()) return { false, message + L" 원래 설정은 복구했습니다." };
            return { false, message + L" 복구 확인 필요: " + lastError_ };
        }

        std::vector<PendingRestore> pending_;
        std::wstring lastError_;
    };

    ThreadPolicyController::ThreadPolicyController() // 스레드 정책·원복 관리자 생성
        : impl_(std::make_unique<Impl>())
    {
    }

    ThreadPolicyController::~ThreadPolicyController() = default; // 내부 스레드 정책 상태 해제

    bool ThreadPolicyController::HasPendingRecovery() const noexcept // 복구 필요 스레드 설정 여부 확인
    {
        return impl_->HasPendingRecovery();
    }

    const std::wstring& ThreadPolicyController::LastError() const noexcept // 최근 스레드 설정 오류 조회
    {
        return impl_->LastError();
    }

    ApplyResult ThreadPolicyController::Apply(HANDLE process, DWORD processId,
        std::uint64_t processCreationTime, const std::wstring& processPath,
        const CpuPlan& plan, const ThreadIdentity& dominantThread,
        const std::wstring& journalPath,
        const std::atomic<bool>& stopRequested) // 확정 지배 스레드·나머지 스레드 CPU 정책 적용
    {
        return impl_->Apply(process, processId, processCreationTime, processPath,
            plan, dominantThread, journalPath, stopRequested);
    }

    bool ThreadPolicyController::TryRestore() // 저장된 스레드 설정 복구·검증
    {
        return impl_->TryRestore();
    }
}
