#include "ProcessAffinityController.h"
#include "RecoveryJournal.h"

#include <TlHelp32.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <vector>

namespace gameoptimizer::engine
{
    namespace
    {
        bool ReadProcessIdentity(HANDLE process, DWORD expectedProcessId,
            std::uint64_t& creationTime) noexcept // PID·생성 시각 기반 프로세스 식별
        {
            if (GetProcessId(process) != expectedProcessId)
                return false;

            FILETIME creation{}, exit{}, kernel{}, user{};
            if (!GetProcessTimes(process, &creation, &exit, &kernel, &user))
                return false;

            creationTime = FileTimeValue(creation);
            return true;
        }

        bool IsCriticalProcess(HANDLE process) noexcept // Windows 중요 프로세스 확인
        {
            BOOL critical = FALSE;
            return IsProcessCritical(process, &critical) && critical != FALSE;
        }

        UniqueHandle DuplicateProcess(HANDLE process) // 복구용 게임 프로세스 핸들 복제
        {
            HANDLE duplicate = nullptr;
            if (!DuplicateHandle(GetCurrentProcess(), process, GetCurrentProcess(),
                &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS))
                return UniqueHandle{};
            return UniqueHandle(duplicate);
        }

        UniqueHandle OpenProcessForAffinity(DWORD processId, HANDLE gameProcess,
            DWORD gameProcessId) // CPU 범위 변경용 프로세스 핸들 열기
        {
            if (processId == gameProcessId)
                return DuplicateProcess(gameProcess);

            return UniqueHandle(OpenProcess(
                PROCESS_QUERY_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION |
                    PROCESS_SET_INFORMATION | SYNCHRONIZE,
                FALSE, processId));
        }

        bool SameProcessIdentity(HANDLE process, DWORD processId,
            std::uint64_t expectedCreationTime) noexcept // 저장 프로세스의 PID·생성 시각 확인
        {
            std::uint64_t creationTime = 0;
            return ReadProcessIdentity(process, processId, creationTime) &&
                creationTime == expectedCreationTime;
        }

        bool SameAffinity(const GROUP_AFFINITY& left,
            const GROUP_AFFINITY& right) noexcept // 스레드 그룹·마스크 비교
        {
            return left.Group == right.Group && left.Mask == right.Mask;
        }

        bool MatchesThreadIdentity(HANDLE thread, DWORD processId, DWORD threadId,
            std::uint64_t expectedCreationTime) noexcept // 복구 스레드 PID·TID·생성 시각 확인
        {
            if (GetThreadId(thread) != threadId || GetProcessIdOfThread(thread) != processId)
                return false;

            FILETIME creation{}, exit{}, kernel{}, user{};
            return GetThreadTimes(thread, &creation, &exit, &kernel, &user) &&
                FileTimeValue(creation) == expectedCreationTime;
        }

    }

    bool ProcessAffinityController::HasPendingRecovery() const noexcept // 프로세스·스레드 affinity 원복 여부 확인
    {
        return std::any_of(pending_.begin(), pending_.end(),
            [](const PendingRestore& item)
            {
                return item.NeedsRestore || !item.Threads.empty();
            });
    }

    bool ProcessAffinityController::CaptureThreadAffinities(DWORD processId,
        KAFFINITY processMask, std::vector<PendingThreadRestore>& threads,
        DWORD& errorCode, std::wstring& error) // 프로세스 범위 변경 전 스레드 값 저장
    {
        errorCode = ERROR_SUCCESS;
        UniqueHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0));
        if (!snapshot.IsValid())
        {
            errorCode = GetLastError();
            error = ErrorMessage(L"프로세스 스레드 목록을 읽지 못했습니다", errorCode);
            return false;
        }

        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        BOOL hasEntry = Thread32First(snapshot.Get(), &entry);
        if (!hasEntry && GetLastError() != ERROR_NO_MORE_FILES)
        {
            errorCode = GetLastError();
            error = ErrorMessage(L"프로세스 스레드 목록을 읽지 못했습니다", errorCode);
            return false;
        }

        while (hasEntry)
        {
            if (entry.th32OwnerProcessID == processId)
            {
                UniqueHandle thread(OpenThread(
                    THREAD_QUERY_INFORMATION | THREAD_SET_INFORMATION | SYNCHRONIZE,
                    FALSE, entry.th32ThreadID));
                if (!thread.IsValid())
                {
                    const DWORD openError = GetLastError();
                    if (openError != ERROR_INVALID_PARAMETER)
                    {
                        errorCode = openError;
                        error = ErrorMessage(L"프로세스 스레드의 CPU 범위를 저장할 수 없습니다",
                            errorCode);
                        return false;
                    }
                }
                else if (GetProcessIdOfThread(thread.Get()) == processId)
                {
                    FILETIME creation{}, exit{}, kernel{}, user{};
                    GROUP_AFFINITY affinity{};
                    bool threadRead = GetThreadTimes(thread.Get(), &creation, &exit, &kernel, &user) != FALSE;
                    DWORD threadError = threadRead ? ERROR_SUCCESS : GetLastError();
                    if (threadRead)
                    {
                        threadRead = GetThreadGroupAffinity(thread.Get(), &affinity) != FALSE;
                        if (!threadRead) threadError = GetLastError();
                    }

                    if (!threadRead)
                    {
                        if (WaitForSingleObject(thread.Get(), 0) != WAIT_OBJECT_0)
                        {
                            errorCode = threadError;
                            error = ErrorMessage(L"프로세스 스레드의 CPU 범위를 저장할 수 없습니다",
                                errorCode);
                            return false;
                        }
                    }
                    else
                    {
                        if (affinity.Group != 0 || affinity.Mask != processMask)
                        {
                            threads.emplace_back(std::move(thread), entry.th32ThreadID,
                                FileTimeValue(creation), affinity);
                        }
                    }
                }
            }

            entry.dwSize = sizeof(entry);
            hasEntry = Thread32Next(snapshot.Get(), &entry);
            if (!hasEntry && GetLastError() != ERROR_NO_MORE_FILES)
            {
                errorCode = GetLastError();
                error = ErrorMessage(L"프로세스 스레드 목록을 끝까지 읽지 못했습니다",
                    errorCode);
                return false;
            }
        }

        return true;
    }

    Result<AffinityApplyResult> ProcessAffinityController::Apply(HANDLE gameProcess, DWORD gameProcessId,
        std::uint64_t gameCreationTime, const std::wstring& gamePath,
        const CpuPlan& plan, const std::wstring& journalPath,
        const std::atomic<bool>& stopRequested) // 게임에 CPU 2곳 배정·다른 프로세스에서 제외
    {
        std::wstring message;
        const auto fail = [&message]() -> Result<AffinityApplyResult>
        {
            return FailText(std::move(message));
        };

        std::erase_if(pending_, [](const PendingRestore& item)
        {
            return WaitForSingleObject(item.Process.Get(), 0) == WAIT_OBJECT_0;
        });

        if (!ValidateTarget(gameProcess, gameProcessId, gameCreationTime, gamePath, message))
            return fail();

        if (plan.Reserved.Group != 0 || plan.Game.Group != 0 ||
            plan.Reserved.Mask == 0 || (plan.Game.Mask & ~plan.Reserved.Mask) != 0)
        {
            message = L"CPU 계획의 예약 범위가 올바르지 않습니다.";
            return fail();
        }

        UniqueHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
        if (!snapshot.IsValid())
        {
            message = ErrorMessage(L"프로세스 목록을 읽지 못했습니다");
            return fail();
        }

        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        BOOL hasEntry = Process32FirstW(snapshot.Get(), &entry);
        if (!hasEntry && GetLastError() != ERROR_NO_MORE_FILES)
        {
            message = ErrorMessage(L"프로세스 목록을 읽지 못했습니다");
            return fail();
        }

        bool foundGame = false;
        std::size_t restrictedProcessCount = 0;
        std::vector<std::wstring> restrictedProcesses;
        const auto recordRestricted = [&restrictedProcessCount, &restrictedProcesses](
            const std::wstring& reason, const PROCESSENTRY32W& processEntry)
        {
            ++restrictedProcessCount;
            if (restrictedProcesses.size() < 3)
            {
                restrictedProcesses.push_back(reason + L": " + processEntry.szExeFile +
                    L" (PID " + std::to_wstring(processEntry.th32ProcessID) + L")");
            }
        };
        const auto describeProcess = [](const std::wstring& message,
            const PROCESSENTRY32W& processEntry)
        {
            return message + L" " + processEntry.szExeFile + L" (PID " +
                std::to_wstring(processEntry.th32ProcessID) + L").";
        };
        while (hasEntry)
        {
            if (stopRequested.load())
                return AffinityApplyResult{ restrictedProcessCount, std::move(restrictedProcesses) };

            const DWORD processId = entry.th32ProcessID;
            if (processId > 4)
            {
                bool skipCurrentProcess = false;
                UniqueHandle process = OpenProcessForAffinity(processId,
                    gameProcess, gameProcessId);
                if (!process.IsValid())
                {
                    const DWORD openError = GetLastError();
                    if (openError == ERROR_ACCESS_DENIED && processId != gameProcessId)
                    {
                        recordRestricted(L"접근 거부(프로세스 열기)", entry);
                    }
                    else if (openError != ERROR_INVALID_PARAMETER)
                    {
                        message = describeProcess(ErrorMessage(
                            L"프로세스 CPU 범위를 변경할 수 없습니다", openError), entry);
                        return fail();
                    }
                }
                else if (IsCriticalProcess(process.Get()))
                {
                    recordRestricted(L"Windows 중요 프로세스", entry);
                }
                else
                {
                    std::uint64_t creationTime = 0;
                    if (!ReadProcessIdentity(process.Get(), processId, creationTime))
                    {
                        const DWORD identityError = GetLastError();
                        const DWORD wait = WaitForSingleObject(process.Get(), 0);
                        if (wait != WAIT_OBJECT_0)
                        {
                            if (identityError == ERROR_ACCESS_DENIED && processId != gameProcessId)
                                recordRestricted(L"접근 거부(프로세스 확인)", entry);
                            else
                            {
                                message = describeProcess(ErrorMessage(
                                    L"프로세스 신원을 확인할 수 없습니다", identityError), entry);
                                return fail();
                            }
                        }
                    }
                    else
                    {
                        const bool isGame = processId == gameProcessId &&
                            creationTime == gameCreationTime;
                        if (processId == gameProcessId && !isGame)
                        {
                            message = L"게임 PID의 생성 시각이 달라졌습니다.";
                            return fail();
                        }
                        if (isGame) foundGame = true;

                        KAFFINITY currentMask = 0;
                        KAFFINITY systemMask = 0;
                        if (!GetProcessAffinityMask(process.Get(), &currentMask, &systemMask))
                        {
                            const DWORD queryError = GetLastError();
                            if (queryError == ERROR_ACCESS_DENIED && !isGame)
                                recordRestricted(L"접근 거부(CPU 범위 조회)", entry);
                            else
                            {
                                message = describeProcess(ErrorMessage(
                                    L"프로세스 CPU 범위를 읽지 못했습니다", queryError), entry);
                                return fail();
                            }
                        }
                        else
                        {
                            const KAFFINITY remainingMask = currentMask & ~plan.Reserved.Mask;
                            const KAFFINITY desiredMask = isGame
                                ? plan.Game.Mask
                                : (remainingMask != 0 ? remainingMask
                                    : (systemMask & ~plan.Reserved.Mask));
                            if (desiredMask == 0 || (desiredMask & ~systemMask) != 0)
                            {
                                message = describeProcess(
                                    L"프로세스에 사용할 CPU 범위를 만들지 못했습니다.", entry);
                                return fail();
                            }

                            auto pending = std::find_if(pending_.begin(), pending_.end(),
                                [processId, creationTime](const PendingRestore& item)
                                {
                                    return item.ProcessId == processId &&
                                        item.CreationFileTime == creationTime;
                                });

                            KAFFINITY originalMask = currentMask;
                            if (pending != pending_.end())
                            {
                                originalMask = pending->OriginalMask;
                                if (currentMask != pending->AppliedMask && currentMask != originalMask)
                                {
                                    message = describeProcess(
                                        L"프로세스 CPU 범위가 외부에서 변경되었습니다.", entry);
                                    return fail();
                                }
                            }

                            if (currentMask != desiredMask)
                            {
                                const bool hadPending = pending != pending_.end();
                                const KAFFINITY previousAppliedMask = hadPending
                                    ? pending->AppliedMask : 0;
                                const bool previousNeedsRestore = hadPending && pending->NeedsRestore;
                                bool createdPending = false;

                                if (!hadPending)
                                {
                                    std::vector<PendingThreadRestore> originalThreads;
                                    DWORD captureError = ERROR_SUCCESS;
                                    if (!CaptureThreadAffinities(processId, currentMask,
                                        originalThreads, captureError, message))
                                    {
                                        if (captureError == ERROR_ACCESS_DENIED && !isGame)
                                        {
                                            recordRestricted(L"접근 거부(스레드 범위 저장)", entry);
                                            skipCurrentProcess = true;
                                        }
                                        else
                                        {
                                            message = describeProcess(message, entry);
                                            return fail();
                                        }
                                    }
                                    else
                                    {
                                        for (const auto& thread : originalThreads)
                                        {
                                            if (!RecordImplicitThreadAffinity(journalPath,
                                                processId, creationTime, thread.ThreadId,
                                                thread.CreationFileTime, thread.OriginalAffinity,
                                                currentMask, desiredMask, message))
                                                return fail();
                                        }
                                        if (!RecordProcessAffinity(journalPath, processId,
                                            creationTime, currentMask, desiredMask, message))
                                            return fail();
                                        pending_.emplace_back(std::move(process), processId,
                                            creationTime, currentMask, desiredMask,
                                            std::move(originalThreads));
                                        pending = std::prev(pending_.end());
                                        createdPending = true;
                                    }
                                }
                                else
                                {
                                    if (!RecordProcessAffinity(journalPath, processId,
                                        creationTime, currentMask, desiredMask, message))
                                        return fail();
                                    pending->AppliedMask = desiredMask;
                                    pending->NeedsRestore = pending->OriginalMask != desiredMask;
                                }

                                if (!skipCurrentProcess &&
                                    !SetProcessAffinityMask(pending->Process.Get(), desiredMask))
                                {
                                    const DWORD applyError = GetLastError();
                                    if (applyError == ERROR_ACCESS_DENIED && !isGame)
                                    {
                                        if (createdPending)
                                            pending_.erase(pending);
                                        else
                                        {
                                            pending->AppliedMask = previousAppliedMask;
                                            pending->NeedsRestore = previousNeedsRestore;
                                        }
                                        recordRestricted(L"접근 거부(CPU 범위 적용)", entry);
                                        skipCurrentProcess = true;
                                    }
                                    else
                                    {
                                        message = describeProcess(ErrorMessage(
                                            L"프로세스 CPU 범위를 적용하지 못했습니다", applyError), entry);
                                        return fail();
                                    }
                                }

                                if (!skipCurrentProcess)
                                {
                                    KAFFINITY verifiedMask = 0;
                                    KAFFINITY verifiedSystemMask = 0;
                                    if (!GetProcessAffinityMask(pending->Process.Get(),
                                        &verifiedMask, &verifiedSystemMask) || verifiedMask != desiredMask)
                                    {
                                        message = describeProcess(
                                            L"프로세스 CPU 범위 적용 후 읽기 검증에 실패했습니다.", entry);
                                        return fail();
                                    }
                                }
                            }
                            else if (pending != pending_.end())
                            {
                                pending->AppliedMask = desiredMask;
                                pending->NeedsRestore = pending->OriginalMask != desiredMask;
                            }
                        }
                    }
                }
            }

            entry.dwSize = sizeof(entry);
            hasEntry = Process32NextW(snapshot.Get(), &entry);
            if (!hasEntry && GetLastError() != ERROR_NO_MORE_FILES)
            {
                message = ErrorMessage(L"프로세스 목록을 끝까지 읽지 못했습니다");
                return fail();
            }
        }

        if (!foundGame)
        {
            message = L"게임 프로세스를 전체 프로세스 목록에서 찾지 못했습니다.";
            return fail();
        }

        lastError_.clear();
        return AffinityApplyResult{ restrictedProcessCount, std::move(restrictedProcesses) };
    }

    bool ProcessAffinityController::TryRestore() // 저장된 프로세스·스레드 affinity 복구·검증
    {
        lastError_.clear();
        auto item = pending_.begin();
        while (item != pending_.end())
        {
            const DWORD wait = WaitForSingleObject(item->Process.Get(), 0);
            if (wait == WAIT_OBJECT_0)
            {
                item = pending_.erase(item);
                continue;
            }
            if (wait != WAIT_TIMEOUT || !SameProcessIdentity(item->Process.Get(),
                item->ProcessId, item->CreationFileTime))
            {
                lastError_ = L"복구 대상 프로세스의 신원을 확인할 수 없습니다.";
                ++item;
                continue;
            }

            KAFFINITY currentMask = 0;
            KAFFINITY systemMask = 0;
            if (!GetProcessAffinityMask(item->Process.Get(), &currentMask, &systemMask))
            {
                lastError_ = ErrorMessage(L"복구 전 프로세스 CPU 범위를 읽지 못했습니다");
                ++item;
                continue;
            }

            if (currentMask == item->OriginalMask)
            {
                item->NeedsRestore = false;
            }
            else if (currentMask != item->AppliedMask)
            {
                lastError_ = L"프로세스 CPU 범위가 외부에서 변경되어 안전한 원복을 확인할 수 없습니다.";
                ++item;
                continue;
            }
            else if (item->NeedsRestore)
            {
                KAFFINITY verifiedMask = 0;
                KAFFINITY verifiedSystemMask = 0;
                if (!SetProcessAffinityMask(item->Process.Get(), item->OriginalMask) ||
                    !GetProcessAffinityMask(item->Process.Get(), &verifiedMask, &verifiedSystemMask) ||
                    verifiedMask != item->OriginalMask)
                {
                    lastError_ = L"원래 프로세스 CPU 범위를 복구하거나 검증하지 못했습니다.";
                    ++item;
                    continue;
                }
                item->NeedsRestore = false;
            }

            GROUP_AFFINITY processOriginal{};
            processOriginal.Group = 0;
            processOriginal.Mask = item->OriginalMask;
            GROUP_AFFINITY processApplied{};
            processApplied.Group = 0;
            processApplied.Mask = item->AppliedMask;

            auto thread = item->Threads.begin();
            while (thread != item->Threads.end())
            {
                const DWORD threadWait = WaitForSingleObject(thread->Thread.Get(), 0);
                if (threadWait == WAIT_OBJECT_0)
                {
                    thread = item->Threads.erase(thread);
                    continue;
                }
                if (threadWait != WAIT_TIMEOUT || !MatchesThreadIdentity(
                    thread->Thread.Get(), item->ProcessId, thread->ThreadId,
                    thread->CreationFileTime))
                {
                    lastError_ = L"복구 대상 프로세스 스레드의 신원을 확인할 수 없습니다.";
                    ++thread;
                    continue;
                }

                GROUP_AFFINITY current{};
                if (!GetThreadGroupAffinity(thread->Thread.Get(), &current))
                {
                    lastError_ = L"복구 전 프로세스 스레드 CPU 범위를 읽지 못했습니다.";
                    ++thread;
                    continue;
                }
                if (SameAffinity(current, thread->OriginalAffinity))
                {
                    thread = item->Threads.erase(thread);
                    continue;
                }
                if (!SameAffinity(current, processOriginal) &&
                    !SameAffinity(current, processApplied))
                {
                    lastError_ = L"프로세스 스레드 CPU 범위가 외부에서 변경되어 안전한 원복을 확인할 수 없습니다.";
                    ++thread;
                    continue;
                }
                if (!SetThreadGroupAffinity(thread->Thread.Get(),
                    &thread->OriginalAffinity, nullptr) ||
                    !GetThreadGroupAffinity(thread->Thread.Get(), &current) ||
                    !SameAffinity(current, thread->OriginalAffinity))
                {
                    lastError_ = L"원래 프로세스 스레드 CPU 범위를 복구하거나 검증하지 못했습니다.";
                    ++thread;
                    continue;
                }
                thread = item->Threads.erase(thread);
            }

            if (!item->NeedsRestore && item->Threads.empty())
                item = pending_.erase(item);
            else
                ++item;
        }

        return !HasPendingRecovery();
    }
}
