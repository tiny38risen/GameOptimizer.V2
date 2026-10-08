#include <Windows.h>
#include <TlHelp32.h>

#include "EngineApi.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    constexpr auto SampleInterval = std::chrono::milliseconds(900);
    constexpr auto ConfirmationTime = std::chrono::seconds(4);
    constexpr int MinimumWindows = 4;

    void SetMessage(GO_STATUS& status, const std::wstring& message) noexcept
    {
        wcsncpy_s(status.Message, _countof(status.Message), message.c_str(), _TRUNCATE);
    }

    void InitializeStatus(GO_STATUS& status, bool passed, const std::wstring& message) noexcept
    {
        status = {};
        status.Passed = passed ? 1U : 0U;
        SetMessage(status, message);
    }

    std::uint64_t FileTimeValue(const FILETIME& value) noexcept
    {
        ULARGE_INTEGER result{};
        result.LowPart = value.dwLowDateTime;
        result.HighPart = value.dwHighDateTime;
        return result.QuadPart;
    }

    std::wstring ErrorMessage(const wchar_t* message, DWORD error = GetLastError())
    {
        return std::wstring(message) + L" (" + std::to_wstring(error) + L").";
    }

    class UniqueHandle
    {
    public:
        explicit UniqueHandle(HANDLE handle = nullptr) noexcept : handle_(handle) {}
        ~UniqueHandle() { Reset(); }

        UniqueHandle(const UniqueHandle&) = delete;
        UniqueHandle& operator=(const UniqueHandle&) = delete;

        UniqueHandle(UniqueHandle&& other) noexcept : handle_(other.Release()) {}
        UniqueHandle& operator=(UniqueHandle&& other) noexcept
        {
            if (this != &other)
            {
                Reset();
                handle_ = other.Release();
            }
            return *this;
        }

        HANDLE Get() const noexcept { return handle_; }
        bool IsValid() const noexcept { return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE; }

        HANDLE Release() noexcept
        {
            HANDLE result = handle_;
            handle_ = nullptr;
            return result;
        }

        void Reset(HANDLE handle = nullptr) noexcept
        {
            if (IsValid()) CloseHandle(handle_);
            handle_ = handle;
        }

    private:
        HANDLE handle_ = nullptr;
    };

    struct ThreadIdentity
    {
        DWORD ProcessId = 0;
        DWORD ThreadId = 0;
        std::uint64_t CreationFileTime = 0;

        bool operator==(const ThreadIdentity& other) const noexcept
        {
            return ProcessId == other.ProcessId && ThreadId == other.ThreadId &&
                CreationFileTime == other.CreationFileTime;
        }
    };

    struct ThreadIdentityHash
    {
        std::size_t operator()(const ThreadIdentity& identity) const noexcept
        {
            const auto processHash = std::hash<DWORD>{}(identity.ProcessId);
            const auto threadHash = std::hash<DWORD>{}(identity.ThreadId);
            const auto creationHash = std::hash<std::uint64_t>{}(identity.CreationFileTime);
            return processHash ^ (threadHash << 1) ^ (creationHash << 2);
        }
    };

    bool ValidateTarget(
        HANDLE process,
        DWORD processId,
        std::uint64_t expectedCreationTime,
        const std::wstring& expectedPath,
        std::wstring& error)
    {
        DWORD exitCode = 0;
        if (GetProcessId(process) != processId ||
            !GetExitCodeProcess(process, &exitCode) || exitCode != STILL_ACTIVE)
        {
            error = L"선택한 게임 프로세스가 종료되었거나 상태를 읽지 못했습니다.";
            return false;
        }

        FILETIME creation{}, exit{}, kernel{}, user{};
        if (!GetProcessTimes(process, &creation, &exit, &kernel, &user) ||
            FileTimeValue(creation) != expectedCreationTime)
        {
            error = L"선택한 PID의 프로세스 생성 시각이 달라졌습니다.";
            return false;
        }

        if (!expectedPath.empty())
        {
            std::vector<wchar_t> path(32768);
            DWORD pathLength = static_cast<DWORD>(path.size());
            if (!QueryFullProcessImageNameW(process, 0, path.data(), &pathLength))
            {
                error = ErrorMessage(L"게임 실행 경로를 다시 확인하지 못했습니다");
                return false;
            }

            if (CompareStringOrdinal(path.data(), static_cast<int>(pathLength),
                expectedPath.c_str(), static_cast<int>(expectedPath.size()), TRUE) != CSTR_EQUAL)
            {
                error = L"선택한 PID의 실행 파일 경로가 달라졌습니다.";
                return false;
            }
        }

        (void)processId; // The retained handle identifies the process; its creation time is checked above.
        return true;
    }

    struct CandidateObservation
    {
        bool HasCandidate = false;
        bool Confirmed = false;
        ThreadIdentity Candidate{};
        std::wstring Detail;
    };

    class ThreadCandidateTracker
    {
    public:
        CandidateObservation Observe(DWORD processId)
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
            for (const auto& [identity, cpuTime] : currentCpu)
            {
                const auto previous = previousCpu_.find(identity);
                const std::uint64_t delta = previous != previousCpu_.end() && cpuTime >= previous->second
                    ? cpuTime - previous->second : 0;
                deltas.push_back({ identity, delta });
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
                confirmed ? L"안정 후보" : L"안정성 확인 중" };
        }

    private:
        void Reset() noexcept
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

    struct ApplyResult
    {
        bool Success = false;
        bool RecoveryRequired = false;
        std::wstring Message;
    };

    class ThreadPolicyController
    {
        struct PendingRestore
        {
            UniqueHandle Thread;
            ThreadIdentity Identity;
            GROUP_AFFINITY OriginalAffinity{};
            GROUP_AFFINITY AppliedAffinity{};
            int OriginalPriority = 0;
            int AppliedPriority = 0;
            bool NeedsAffinityRestore = false;
            bool NeedsPriorityRestore = false;

            PendingRestore(UniqueHandle&& thread, const ThreadIdentity& identity,
                const GROUP_AFFINITY& originalAffinity, int originalPriority,
                bool needsAffinity, const GROUP_AFFINITY& appliedAffinity,
                bool needsPriority, int appliedPriority)
                : Thread(std::move(thread)), Identity(identity),
                  OriginalAffinity(originalAffinity), AppliedAffinity(appliedAffinity),
                  OriginalPriority(originalPriority), AppliedPriority(appliedPriority),
                  NeedsAffinityRestore(needsAffinity), NeedsPriorityRestore(needsPriority)
            {
            }
        };

    public:
        bool HasPendingRecovery() const noexcept { return pending_.has_value(); }
        const std::wstring& LastError() const noexcept { return lastError_; }

        ApplyResult Apply(HANDLE process, DWORD processId, std::uint64_t processCreationTime,
            const std::wstring& processPath, const ThreadIdentity& identity,
            const std::atomic<bool>& stopRequested)
        {
            if (HasPendingRecovery())
                return { false, true, L"이전 후보의 복구가 끝나지 않아 새 스레드에 적용하지 않았습니다." };

            std::wstring error;
            if (!ValidateTarget(process, processId, processCreationTime, processPath, error))
                return { false, false, error };

            UniqueHandle thread(OpenThread(
                THREAD_QUERY_INFORMATION | THREAD_SET_INFORMATION | SYNCHRONIZE,
                FALSE, identity.ThreadId));
            if (!thread.IsValid())
                return { false, false, ErrorMessage(L"후보 스레드에 접근할 수 없습니다") };
            if (!MatchesIdentity(thread.Get(), identity))
                return { false, false, L"후보 스레드가 관찰 이후 종료되거나 재사용되었습니다." };

            GROUP_AFFINITY originalAffinity{};
            if (!GetThreadGroupAffinity(thread.Get(), &originalAffinity))
                return { false, false, ErrorMessage(L"후보 스레드의 CPU 배치를 읽지 못했습니다") };

            const int originalPriority = GetThreadPriority(thread.Get());
            if (originalPriority == THREAD_PRIORITY_ERROR_RETURN)
                return { false, false, ErrorMessage(L"후보 스레드의 우선순위를 읽지 못했습니다") };

            GROUP_AFFINITY appliedAffinity = originalAffinity;
            if (!SelectLogicalProcessors(originalAffinity.Mask, appliedAffinity.Mask))
                return { false, false, L"후보 스레드의 허용 CPU 수가 두 개보다 적습니다." };
            const int appliedPriority = THREAD_PRIORITY_ABOVE_NORMAL;
            const bool changeAffinity = !SameAffinity(originalAffinity, appliedAffinity);
            const bool changePriority = originalPriority != appliedPriority;
            if (!changeAffinity && !changePriority)
                return { true, false, L"선택한 설정이 이미 적용되어 있습니다." };

            if (!ValidateTarget(process, processId, processCreationTime, processPath, error))
                return { false, false, error };
            if (!MatchesIdentity(thread.Get(), identity))
                return { false, false, L"적용 직전 후보 스레드 신원이 달라졌습니다." };
            if (stopRequested.load())
                return { false, false, L"적용 전에 중단 요청을 받았습니다." };

            pending_.emplace(std::move(thread), identity, originalAffinity, originalPriority,
                changeAffinity, appliedAffinity, changePriority, appliedPriority);

            if (!ValidateTarget(process, processId, processCreationTime, processPath, error))
                return FailAndAttemptRestore(error);
            if (stopRequested.load())
                return FailAndAttemptRestore(L"설정 적용 중 중단 요청을 받았습니다.");
            if (pending_->NeedsAffinityRestore &&
                !SetThreadGroupAffinity(pending_->Thread.Get(), &pending_->AppliedAffinity, nullptr))
                return FailAndAttemptRestore(ErrorMessage(L"CPU 배치 적용에 실패했습니다"));

            if (!ValidateTarget(process, processId, processCreationTime, processPath, error))
                return FailAndAttemptRestore(error);
            if (stopRequested.load())
                return FailAndAttemptRestore(L"설정 적용 중 중단 요청을 받았습니다.");
            if (pending_->NeedsPriorityRestore &&
                !SetThreadPriority(pending_->Thread.Get(), pending_->AppliedPriority))
                return FailAndAttemptRestore(ErrorMessage(L"후보 우선순위 적용에 실패했습니다"));

            if (!VerifyApplied(*pending_))
                return FailAndAttemptRestore(L"적용 후 읽기 검증에 실패했습니다.");

            return { true, false, L"후보 설정을 적용하고 읽기 검증했습니다." };
        }

        bool TryRestore()
        {
            if (!pending_)
            {
                lastError_.clear();
                return true;
            }

            const DWORD wait = WaitForSingleObject(pending_->Thread.Get(), 0);
            if (wait == WAIT_OBJECT_0)
            {
                pending_.reset();
                lastError_.clear();
                return true;
            }
            if (wait != WAIT_TIMEOUT)
            {
                lastError_ = L"후보 스레드의 생존 상태를 확인할 수 없습니다.";
                return false;
            }

            if (!MatchesIdentity(pending_->Thread.Get(), pending_->Identity))
            {
                lastError_ = L"복구 대상의 TID·PID·생성 시각이 원본과 일치하지 않습니다.";
                return false;
            }

            if (pending_->NeedsAffinityRestore)
            {
                GROUP_AFFINITY current{};
                if (!GetThreadGroupAffinity(pending_->Thread.Get(), &current))
                {
                    lastError_ = L"복구 전 CPU 배치를 읽지 못했습니다.";
                    return false;
                }

                if (SameAffinity(current, pending_->OriginalAffinity))
                {
                    pending_->NeedsAffinityRestore = false;
                }
                else if (!SameAffinity(current, pending_->AppliedAffinity))
                {
                    lastError_ = L"CPU 배치가 외부에서 변경되어 안전한 원복을 확인할 수 없습니다.";
                    return false;
                }
                else if (!SetThreadGroupAffinity(pending_->Thread.Get(), &pending_->OriginalAffinity, nullptr) ||
                    !GetThreadGroupAffinity(pending_->Thread.Get(), &current) ||
                    !SameAffinity(current, pending_->OriginalAffinity))
                {
                    lastError_ = L"원래 CPU 배치를 복구하거나 검증하지 못했습니다.";
                    return false;
                }
                else
                {
                    pending_->NeedsAffinityRestore = false;
                }
            }

            if (pending_->NeedsPriorityRestore)
            {
                const int current = GetThreadPriority(pending_->Thread.Get());
                if (current == THREAD_PRIORITY_ERROR_RETURN)
                {
                    lastError_ = L"복구 전 우선순위를 읽지 못했습니다.";
                    return false;
                }
                if (current == pending_->OriginalPriority)
                {
                    pending_->NeedsPriorityRestore = false;
                }
                else if (current != pending_->AppliedPriority)
                {
                    lastError_ = L"우선순위가 외부에서 변경되어 안전한 원복을 확인할 수 없습니다.";
                    return false;
                }
                else if (!SetThreadPriority(pending_->Thread.Get(), pending_->OriginalPriority) ||
                    GetThreadPriority(pending_->Thread.Get()) != pending_->OriginalPriority)
                {
                    lastError_ = L"원래 우선순위를 복구하거나 검증하지 못했습니다.";
                    return false;
                }
                else
                {
                    pending_->NeedsPriorityRestore = false;
                }
            }

            if (!pending_->NeedsAffinityRestore && !pending_->NeedsPriorityRestore)
            {
                pending_.reset();
                lastError_.clear();
                return true;
            }

            lastError_ = L"복구가 일부만 확인되었습니다.";
            return false;
        }

    private:
        static bool SelectLogicalProcessors(KAFFINITY available, KAFFINITY& selected) noexcept
        {
            int wanted = 2;
            selected = 0;
            for (int bit = 0; bit < static_cast<int>(sizeof(KAFFINITY) * 8) && wanted > 0; ++bit)
            {
                const KAFFINITY candidate = static_cast<KAFFINITY>(1) << bit;
                if ((available & candidate) == 0) continue;
                selected |= candidate;
                --wanted;
            }
            return wanted == 0;
        }

        static bool SameAffinity(const GROUP_AFFINITY& left, const GROUP_AFFINITY& right) noexcept
        {
            return left.Group == right.Group && left.Mask == right.Mask;
        }

        static bool MatchesIdentity(HANDLE thread, const ThreadIdentity& identity) noexcept
        {
            if (GetThreadId(thread) != identity.ThreadId ||
                GetProcessIdOfThread(thread) != identity.ProcessId)
                return false;

            FILETIME creation{}, exit{}, kernel{}, user{};
            return GetThreadTimes(thread, &creation, &exit, &kernel, &user) &&
                FileTimeValue(creation) == identity.CreationFileTime;
        }

        static bool VerifyApplied(const PendingRestore& state) noexcept
        {
            if (state.NeedsAffinityRestore)
            {
                GROUP_AFFINITY current{};
                if (!GetThreadGroupAffinity(state.Thread.Get(), &current) ||
                    !SameAffinity(current, state.AppliedAffinity))
                    return false;
            }
            return !state.NeedsPriorityRestore ||
                GetThreadPriority(state.Thread.Get()) == state.AppliedPriority;
        }

        ApplyResult FailAndAttemptRestore(const std::wstring& message)
        {
            if (TryRestore()) return { false, false, message + L" 원래 설정은 복구했습니다." };
            return { false, true, message + L" 복구 확인 필요: " + lastError_ };
        }

        std::optional<PendingRestore> pending_;
        std::wstring lastError_;
    };

    struct GO_SESSION_IMPL
    {
        GO_SESSION_IMPL(UniqueHandle process, DWORD processId,
            std::uint64_t processCreationTime, std::wstring processPath)
            : process_(std::move(process)), processId_(processId),
              processCreationTime_(processCreationTime), processPath_(std::move(processPath))
        {
            InitializeStatus(status_, true, L"대상 확인 중…");
            status_.ProcessId = processId_;
        }

        ~GO_SESSION_IMPL()
        {
            RequestStop();
            Join();
        }

        void Start()
        {
            worker_ = std::thread([this] { RunWorker(); });
        }

        void RequestStop() noexcept
        {
            stopRequested_.store(true);
            wake_.notify_all();
        }

        void Join() noexcept
        {
            if (worker_.joinable()) worker_.join();
        }

        int Wait(std::uint32_t timeoutMilliseconds) noexcept
        {
            std::unique_lock lock(statusMutex_);
            const bool complete = completedCondition_.wait_for(lock,
                std::chrono::milliseconds(timeoutMilliseconds), [this] { return completed_; });
            return complete ? 1 : 0;
        }

        void ReadStatus(GO_STATUS& output)
        {
            std::lock_guard lock(statusMutex_);
            output = status_;
        }

        bool RetryRestore()
        {
            std::lock_guard lock(statusMutex_);
            if (!completed_) return false;

            if (!policy_.TryRestore())
            {
                status_.Passed = 0;
                status_.RecoveryRequired = 1;
                SetMessage(status_, L"복구를 확인하지 못했습니다. 새 적용은 차단되어 있습니다. " + policy_.LastError());
                return false;
            }

            status_.RecoveryRequired = 0;
            status_.Passed = resultPassed_ ? 1U : 0U;
            SetMessage(status_, resultPassed_
                ? L"원래 설정으로 복구했습니다."
                : L"실행은 실패했지만 원래 설정 복구를 확인했습니다. " + resultMessage_);
            return true;
        }

        bool HasPendingRecovery() const noexcept { return policy_.HasPendingRecovery(); }

    private:
        void Publish(const CandidateObservation& observation, const std::wstring& message)
        {
            std::lock_guard lock(statusMutex_);
            status_.HasCandidate = observation.HasCandidate ? 1U : 0U;
            status_.Confirmed = observation.Confirmed ? 1U : 0U;
            status_.ProcessId = processId_;
            status_.ThreadId = observation.HasCandidate ? observation.Candidate.ThreadId : 0;
            status_.RecoveryRequired = 0;
            status_.Passed = 1;
            SetMessage(status_, message);
        }

        void WaitForNextSample()
        {
            std::unique_lock lock(statusMutex_);
            wake_.wait_for(lock, SampleInterval, [this] { return stopRequested_.load(); });
        }

        void RunWorker() noexcept
        {
            bool passed = true;
            std::wstring message = L"중단했고 원래 설정 복구를 확인했습니다.";
            std::optional<ThreadIdentity> lastCandidate;

            try
            {
                while (!stopRequested_.load())
                {
                    std::wstring error;
                    if (!ValidateTarget(process_.Get(), processId_, processCreationTime_, processPath_, error))
                    {
                        passed = false;
                        message = error;
                        break;
                    }

                    const CandidateObservation observation = tracker_.Observe(processId_);
                    if (stopRequested_.load()) break;
                    if (observation.HasCandidate) lastCandidate = observation.Candidate;

                    if (observation.HasCandidate && observation.Confirmed)
                    {
                        if (!lastHandledCandidate_ || !(*lastHandledCandidate_ == observation.Candidate))
                        {
                            if (policy_.HasPendingRecovery() && !policy_.TryRestore())
                            {
                                passed = false;
                                message = L"이전 후보의 복구를 확인하지 못해 새 후보를 변경하지 않았습니다: " + policy_.LastError();
                                break;
                            }

                            lastHandledCandidate_ = observation.Candidate;
                            const ApplyResult applied = policy_.Apply(process_.Get(), processId_,
                                processCreationTime_, processPath_, observation.Candidate, stopRequested_);
                            if (!applied.Success)
                            {
                                passed = false;
                                message = applied.Message;
                                break;
                            }
                        }

                        const std::wstring statusMessage = policy_.HasPendingRecovery()
                            ? L"안정 후보의 설정을 적용해 확인했습니다. 중단하면 원복합니다."
                            : L"안정 후보가 이미 선택한 설정을 사용 중입니다.";
                        Publish(observation, statusMessage);
                    }
                    else
                    {
                        if (policy_.HasPendingRecovery())
                        {
                            if (!policy_.TryRestore())
                            {
                                passed = false;
                                message = L"후보 근거가 끊겼고 원래 설정을 복구하지 못했습니다: " + policy_.LastError();
                                break;
                            }
                            lastHandledCandidate_.reset();
                        }

                        const std::wstring statusMessage = observation.HasCandidate
                            ? L"TID " + std::to_wstring(observation.Candidate.ThreadId) + L"의 후보 안정성을 확인 중입니다."
                            : observation.Detail;
                        Publish(observation, statusMessage);
                    }

                    WaitForNextSample();
                }

                if (stopRequested_.load() && passed)
                    message = L"중단했고 원래 설정 복구를 확인했습니다.";
            }
            catch (...)
            {
                passed = false;
                message = L"C++ 엔진 실행 중 오류가 발생했습니다.";
            }

            if (policy_.HasPendingRecovery() && !policy_.TryRestore())
            {
                passed = false;
                message = L"원래 설정 복구를 확인하지 못했습니다. 재시도 전에는 새 적용을 차단합니다: " + policy_.LastError();
            }

            resultPassed_ = passed;
            resultMessage_ = message;
            {
                std::lock_guard lock(statusMutex_);
                completed_ = true;
                status_.HasCandidate = lastCandidate ? 1U : 0U;
                status_.Confirmed = 0;
                status_.RecoveryRequired = policy_.HasPendingRecovery() ? 1U : 0U;
                status_.Passed = passed && !policy_.HasPendingRecovery() ? 1U : 0U;
                status_.ProcessId = processId_;
                status_.ThreadId = lastCandidate ? lastCandidate->ThreadId : 0;
                SetMessage(status_, message);
            }
            completedCondition_.notify_all();
        }

        UniqueHandle process_;
        DWORD processId_;
        std::uint64_t processCreationTime_;
        std::wstring processPath_;
        ThreadCandidateTracker tracker_;
        ThreadPolicyController policy_;
        std::optional<ThreadIdentity> lastHandledCandidate_;
        std::atomic<bool> stopRequested_{ false };
        std::mutex statusMutex_;
        std::condition_variable wake_;
        std::condition_variable completedCondition_;
        GO_STATUS status_{};
        std::thread worker_;
        bool completed_ = false;
        bool resultPassed_ = false;
        std::wstring resultMessage_;
    };
}

struct GO_SESSION
{
    GO_SESSION(UniqueHandle&& process, DWORD processId,
        std::uint64_t processCreationTime, std::wstring processPath)
        : impl(std::move(process), processId, processCreationTime, std::move(processPath))
    {
    }

    GO_SESSION_IMPL impl;
};

extern "C" GO_API GO_SESSION* GO_CALL go_start_session(
    std::uint32_t processId,
    std::uint64_t processCreationFileTime,
    const wchar_t* fullPath,
    GO_STATUS* startupStatus) noexcept
{
    if (!startupStatus) return nullptr;
    InitializeStatus(*startupStatus, false, L"C++ 엔진을 시작하지 못했습니다.");

    try
    {
        UniqueHandle process(OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, processId));
        if (!process.IsValid())
        {
            SetMessage(*startupStatus, ErrorMessage(L"게임 프로세스에 접근할 수 없습니다"));
            return nullptr;
        }

        const std::wstring path = fullPath ? fullPath : L"";
        std::wstring error;
        if (!ValidateTarget(process.Get(), processId, processCreationFileTime, path, error))
        {
            SetMessage(*startupStatus, error);
            return nullptr;
        }

        auto session = std::make_unique<GO_SESSION>(std::move(process), processId,
            processCreationFileTime, path);
        session->impl.Start();
        session->impl.ReadStatus(*startupStatus);
        return session.release();
    }
    catch (...)
    {
        SetMessage(*startupStatus, L"C++ 엔진 시작 중 오류가 발생했습니다.");
        return nullptr;
    }
}

extern "C" GO_API int GO_CALL go_read_status(GO_SESSION* session, GO_STATUS* status) noexcept
{
    if (!session || !status) return 0;
    try
    {
        session->impl.ReadStatus(*status);
        return 1;
    }
    catch (...)
    {
        return 0;
    }
}

extern "C" GO_API int GO_CALL go_request_stop(GO_SESSION* session) noexcept
{
    if (!session) return 0;
    session->impl.RequestStop();
    return 1;
}

extern "C" GO_API int GO_CALL go_wait(GO_SESSION* session, std::uint32_t timeoutMilliseconds) noexcept
{
    if (!session) return -1;
    return session->impl.Wait(timeoutMilliseconds);
}

extern "C" GO_API int GO_CALL go_try_restore(GO_SESSION* session) noexcept
{
    if (!session) return 0;
    try
    {
        return session->impl.RetryRestore() ? 1 : 0;
    }
    catch (...)
    {
        return 0;
    }
}

extern "C" GO_API int GO_CALL go_destroy_session(GO_SESSION* session) noexcept
{
    if (!session) return 1;
    session->impl.RequestStop();
    session->impl.Join();
    if (session->impl.HasPendingRecovery()) return 0;
    delete session;
    return 1;
}
