#include "EngineApi.h"

#include "EngineInternals.h"
#include "CpuPlanner.h"
#include "DiagnosticLog.h"
#include "GameDiscovery.h"
#include "ProcessAffinityController.h"
#include "ProcessPriority.h"
#include "RecoveryJournal.h"
#include "ResidualLoad.h"
#include "ThreadPolicy.h"
#include "ThreadTracker.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

static_assert(sizeof(wchar_t) == 2, "The C ABI requires Windows UTF-16 wchar_t.");
static_assert(offsetof(GO_GAME_TARGET, WindowTitle) == 20);
static_assert(sizeof(GO_GAME_TARGET) == 1568);
static_assert(sizeof(GO_STATUS) == 1052);

namespace gameoptimizer::engine
{
    namespace
    {
        constexpr auto SampleInterval = std::chrono::milliseconds(900);
        constexpr DWORD AutomaticProcessPriorityClass = HIGH_PRIORITY_CLASS;
        constexpr double ResidualWarningPercent = 5.0;

        std::wstring FormatSelectedCore(const CpuCoreCandidate& core) // 선택 코어·논리 CPU 정보 구성
        {
            std::wstring text = std::format(
                L"코어 {}/LP{} (속도 {:.1f}%, 인터럽트+DPC {:.1f}%",
                core.PhysicalCoreIndex, core.LogicalProcessorIndex,
                core.RelativeSpeedPercent, core.InterruptDpcPercent);
            if (core.EfficiencyClass)
                std::format_to(std::back_inserter(text), L", 효율 {}",
                    static_cast<unsigned int>(*core.EfficiencyClass));
            text += L')';
            return text;
        }
    }

    struct GO_SESSION_IMPL
    {
        GO_SESSION_IMPL(UniqueHandle process, DWORD processId,
            std::uint64_t processCreationTime, std::wstring processPath,
            std::wstring journalPath, std::wstring diagnosticsPath) // 대상 프로세스·세션 초기값 보관
            : process_(std::move(process)), processId_(processId),
              processCreationTime_(processCreationTime), processPath_(std::move(processPath)),
              journalPath_(std::move(journalPath)), diagnostics_(std::move(diagnosticsPath))
        {
            InitializeStatus(status_, true, L"대상 확인 중…");
            status_.ProcessId = processId_;
        }

        ~GO_SESSION_IMPL() // 세션 스레드 종료 대기·복구 확인
        {
            RequestStop();
            Join();
        }

        void Start() // 백그라운드 최적화 세션 시작
        {
            worker_ = std::thread([this] { RunWorker(); });
        }

        void RequestStop() noexcept // 세션 중단 신호·대기 해제
        {
            stopRequested_.store(true);
            wake_.notify_all();
        }

        void Join() noexcept // 백그라운드 세션 스레드 종료 대기
        {
            if (worker_.joinable()) worker_.join();
        }

        int Wait(std::uint32_t timeoutMilliseconds) noexcept // 제한 시간 내 세션 완료 대기
        {
            std::unique_lock lock(statusMutex_);
            const bool complete = completedCondition_.wait_for(lock,
                std::chrono::milliseconds(timeoutMilliseconds), [this] { return completed_; });
            return complete ? 1 : 0;
        }

        void ReadStatus(GO_STATUS& output) // 동기화된 세션 상태 복사
        {
            std::lock_guard lock(statusMutex_);
            output = status_;
        }

        bool RetryRestore() // 프로세스·스레드 설정 원복 재검증
        {
            std::lock_guard lock(statusMutex_);
            if (!completed_) return false;

            if (!TryRestorePolicies())
            {
                status_.Passed = 0;
                status_.RecoveryRequired = 1;
                status_.Applied = 1;
                SetMessage(status_, L"복구를 확인하지 못했습니다. 새 적용은 차단되어 있습니다. " + RecoveryError());
                return false;
            }

            status_.RecoveryRequired = 0;
            status_.Applied = 0;
            status_.Passed = resultPassed_ ? 1U : 0U;
            SetMessage(status_, resultPassed_
                ? L"원래 설정으로 복구했습니다."
                : L"실행은 실패했지만 원래 설정 복구를 확인했습니다. " + resultMessage_);
            return true;
        }

        bool HasPendingRecovery() const noexcept // 미완료 정책 원복 여부 확인
        {
            return policy_.HasPendingRecovery() || processAffinity_.HasPendingRecovery() ||
                processPriority_.HasPendingRecovery();
        }

    private:
        void Publish(const CandidateObservation& observation, const std::wstring& message) // 후보·적용 진행 상태 UI 게시
        {
            std::lock_guard lock(statusMutex_);
            status_.HasCandidate = observation.HasCandidate ? 1U : 0U;
            status_.Confirmed = observation.Confirmed ? 1U : 0U;
            status_.Applied = HasPendingRecovery() ? 1U : 0U;
            status_.ProcessId = processId_;
            status_.ThreadId = observation.HasCandidate ? observation.Candidate.ThreadId : 0;
            status_.RecoveryRequired = 0;
            status_.Passed = 1;
            SetMessage(status_, message);
        }

        void WaitForNextSample() // 다음 CPU 표본 대기·중단 확인
        {
            std::unique_lock lock(statusMutex_);
            wake_.wait_for(lock, SampleInterval, [this] { return stopRequested_.load(); });
        }

        void RunWorker() noexcept // CPU 선택·후보 배정·오류 처리·원복
        {
            bool passed = true;
            std::wstring message = L"중단했고 원래 설정 복구를 확인했습니다.";
            std::wstring partialAffinityWarning;
            std::wstring cpuPlanNote;
            std::wstring residualNote;
            std::optional<ThreadIdentity> lastCandidate;
            bool residualChecked = false;

            const auto AddAffinityWarning = [&partialAffinityWarning,
                &cpuPlanNote, &residualNote](std::wstring statusMessage)
            {
                statusMessage = cpuPlanNote + statusMessage;
                if (!partialAffinityWarning.empty())
                    statusMessage += L" " + partialAffinityWarning;
                if (!residualNote.empty())
                    statusMessage += L" " + residualNote;
                return statusMessage;
            };

            try
            {
                diagnostics_.Write(L"target pid={} creation={}",
                    processId_, processCreationTime_);
                diagnostics_.Write(L"session path={}", processPath_);
                auto selectedPlan = SelectCpuPlan(process_.Get(), stopRequested_, diagnostics_);
                if (selectedPlan)
                    cpuPlan_ = std::move(*selectedPlan);
                else if (selectedPlan.error().Code != ERROR_CANCELLED)
                {
                    passed = false;
                    message = selectedPlan.error().Message;
                }

                if (passed && cpuPlan_)
                {
                    cpuPlanNote = L"선택 코어: " + FormatSelectedCore(cpuPlan_->DominantCore) +
                        L" · " + FormatSelectedCore(cpuPlan_->WorkerCore) + L". ";
                    std::wstring affinityMessage;
                    if (!processAffinity_.Apply(process_.Get(), processId_,
                        processCreationTime_, processPath_, *cpuPlan_, journalPath_, stopRequested_,
                        affinityMessage))
                    {
                        passed = false;
                        message = affinityMessage;
                    }
                    else
                    {
                        const std::wstring warningMarker = L"제한으로 건너뛴 프로세스";
                        const std::size_t warningPosition = affinityMessage.find(warningMarker);
                        if (warningPosition != std::wstring::npos)
                            partialAffinityWarning = affinityMessage.substr(warningPosition);
                        message = affinityMessage + L" CPU 지배 스레드를 관찰합니다.";
                        Publish({}, AddAffinityWarning(message));
                    }
                }

                if (passed && cpuPlan_)
                {
                    std::wstring priorityMessage;
                    if (!processPriority_.Apply(process_.Get(), processId_, processCreationTime_,
                        processPath_, AutomaticProcessPriorityClass, journalPath_,
                        stopRequested_, priorityMessage))
                    {
                        passed = false;
                        message = priorityMessage;
                    }
                    else if (!priorityMessage.empty())
                    {
                        message = AddAffinityWarning(priorityMessage + L" CPU 후보를 관찰합니다.");
                        Publish({}, message);
                    }
                }

                while (passed && cpuPlan_ && !stopRequested_.load())
                {
                    std::wstring error;
                    if (!ValidateTarget(process_.Get(), processId_, processCreationTime_, processPath_, error))
                    {
                        passed = false;
                        message = error;
                        break;
                    }

                    partialAffinityWarning.clear();
                    std::wstring affinityMessage;
                    if (!processAffinity_.Apply(process_.Get(), processId_,
                        processCreationTime_, processPath_, *cpuPlan_, journalPath_, stopRequested_,
                        affinityMessage))
                    {
                        passed = false;
                        message = affinityMessage;
                        break;
                    }
                    const std::wstring warningMarker = L"제한으로 건너뛴 프로세스";
                    const std::size_t warningPosition = affinityMessage.find(warningMarker);
                    if (warningPosition != std::wstring::npos)
                        partialAffinityWarning = affinityMessage.substr(warningPosition);
                    if (stopRequested_.load()) break;

                    const CandidateObservation observation = tracker_.Observe(processId_);
                    if (stopRequested_.load()) break;
                    if (observation.HasCandidate) lastCandidate = observation.Candidate;

                    if (observation.HasCandidate && observation.Confirmed)
                    {
                        if (lastHandledCandidate_ && !(*lastHandledCandidate_ == observation.Candidate) &&
                            policy_.HasPendingRecovery())
                        {
                            if (!policy_.TryRestore())
                            {
                                passed = false;
                                message = L"이전 후보의 복구를 확인하지 못해 새 후보를 변경하지 않았습니다: " + policy_.LastError();
                                break;
                            }
                        }

                        const ApplyResult applied = policy_.Apply(process_.Get(), processId_,
                            processCreationTime_, processPath_, *cpuPlan_, observation.Candidate,
                            journalPath_, stopRequested_);
                        if (!applied.Success)
                        {
                            passed = false;
                            message = applied.Message;
                            break;
                        }
                        lastHandledCandidate_ = observation.Candidate;

                        const std::wstring appliedStatus = HasPendingRecovery()
                            ? L"안정 후보를 확인했고 자동 CPU 설정을 적용했습니다. 중단하면 원복합니다."
                            : L"안정 후보가 자동 CPU 설정을 사용 중입니다.";
                        Publish(observation, AddAffinityWarning(appliedStatus));

                        if (!residualChecked)
                        {
                            residualChecked = true;
                            if (const auto residual = MeasureResidualLoad(
                                *cpuPlan_, observation.Candidate, process_.Get(), stopRequested_))
                            {
                                diagnostics_.Write(
                                    L"residual thread={:.1f}% dominantLp={:.1f}% sibling={}",
                                    residual->DominantThreadPercent,
                                    residual->DominantLpResidualPercent,
                                    residual->SiblingPercent
                                        ? std::format(L"{:.1f}%", *residual->SiblingPercent)
                                        : std::wstring(L"none"));
                                const auto& counters = residual->Counters;
                                for (std::size_t lp = 0;
                                    lp < counters.ProcessorTime.size(); ++lp)
                                    diagnostics_.Write(
                                        L"residual lp={} busy={:.1f} int={:.1f} dpc={:.1f}",
                                        lp, counters.ProcessorTime[lp],
                                        counters.InterruptTime[lp], counters.DpcTime[lp]);

                                const double worst = std::max(
                                    residual->DominantLpResidualPercent,
                                    residual->SiblingPercent.value_or(0.0));
                                if (worst > ResidualWarningPercent)
                                {
                                    residualNote = std::format(
                                        L"지배 코어에 옮길 수 없는 부하 {:.1f}%가 있습니다.", worst);
                                    Publish(observation, AddAffinityWarning(appliedStatus));
                                }
                            }
                            else if (residual.error().Code != ERROR_CANCELLED)
                            {
                                diagnostics_.Write(L"residual check failed: {}",
                                    residual.error().Message);
                            }
                            if (stopRequested_.load()) break;
                        }
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

                        const std::wstring statusMessage = AddAffinityWarning(observation.HasCandidate
                            ? L"TID " + std::to_wstring(observation.Candidate.ThreadId) + L"의 후보 안정성을 확인 중입니다."
                            : observation.Detail);
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

            if (!TryRestorePolicies())
            {
                passed = false;
                message = L"원래 설정 복구를 확인하지 못했습니다. 재시도 전에는 새 적용을 차단합니다: " + RecoveryError();
            }

            resultPassed_ = passed;
            resultMessage_ = message;
            {
                std::lock_guard lock(statusMutex_);
                completed_ = true;
                status_.HasCandidate = lastCandidate ? 1U : 0U;
                status_.Confirmed = 0;
                status_.Applied = HasPendingRecovery() ? 1U : 0U;
                status_.RecoveryRequired = HasPendingRecovery() ? 1U : 0U;
                status_.Passed = passed && !HasPendingRecovery() ? 1U : 0U;
                status_.ProcessId = processId_;
                status_.ThreadId = lastCandidate ? lastCandidate->ThreadId : 0;
                SetMessage(status_, message);
            }
            completedCondition_.notify_all();
        }

        bool TryRestorePolicies() // 스레드 배치·CPU 범위·우선순위 복구
        {
            const bool threadRestored = policy_.TryRestore();
            const bool affinityRestored = threadRestored && processAffinity_.TryRestore();
            const bool processRestored = processPriority_.TryRestore(
                process_.Get(), processId_, processCreationTime_, processPath_);
            const bool restored = threadRestored && affinityRestored && processRestored;
            if (restored) RemoveRecoveryJournal(journalPath_);
            return restored;
        }

        std::wstring RecoveryError() const // 미복구 정책 오류 통합
        {
            std::wstring error;
            if (policy_.HasPendingRecovery()) error = policy_.LastError();
            if (processAffinity_.HasPendingRecovery())
            {
                if (!error.empty()) error += L" ";
                error += processAffinity_.LastError();
            }
            if (processPriority_.HasPendingRecovery())
            {
                if (!error.empty()) error += L" ";
                error += processPriority_.LastError();
            }
            return error;
        }

        UniqueHandle process_;
        DWORD processId_;
        std::uint64_t processCreationTime_;
        std::wstring processPath_;
        std::wstring journalPath_;
        DiagnosticLog diagnostics_;
        ThreadCandidateTracker tracker_;
        ProcessAffinityController processAffinity_;
        ProcessPriorityController processPriority_;
        ThreadPolicyController policy_;
        std::optional<CpuPlan> cpuPlan_;
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
    GO_SESSION(gameoptimizer::engine::UniqueHandle&& process, DWORD processId,
        std::uint64_t processCreationTime, std::wstring processPath,
        std::wstring journalPath, std::wstring diagnosticsPath) // 세션 구현체에 대상 프로세스 정보 전달
        : impl(std::move(process), processId, processCreationTime,
            std::move(processPath), std::move(journalPath), std::move(diagnosticsPath))
    {
    }

    gameoptimizer::engine::GO_SESSION_IMPL impl;
};

extern "C" GO_API int GO_CALL go_list_visible_games(
    GO_GAME_TARGET* targets,
    std::uint32_t capacity,
    std::uint32_t* requiredCount) noexcept // 보이는 앱 대상 목록 C ABI 반환
{
    if (!requiredCount || (!targets && capacity != 0)) return 0;
    *requiredCount = 0;

    try
    {
        const auto games = gameoptimizer::engine::FindVisibleGames();
        if (games.size() > UINT32_MAX) return 0;

        *requiredCount = static_cast<std::uint32_t>(games.size());
        const auto count = std::min<std::size_t>(games.size(), capacity);
        if (targets && count > 0)
            std::copy_n(games.begin(), count, targets);
        return 1;
    }
    catch (...)
    {
        *requiredCount = 0;
        return 0;
    }
}

extern "C" GO_API GO_SESSION* GO_CALL go_start_session(
    std::uint32_t processId,
    std::uint64_t processCreationFileTime,
    std::uint32_t requirePathVerification,
    const wchar_t* journalPath,
    const wchar_t* diagnosticsPath,
    GO_STATUS* startupStatus) noexcept // PID·생성 시각·경로 검증 및 최적화 세션 생성
{
    if (!startupStatus || !journalPath || !*journalPath) return nullptr;
    gameoptimizer::engine::InitializeStatus(*startupStatus, false, L"C++ 엔진을 시작하지 못했습니다.");

    try
    {
        DWORD processAccess = PROCESS_QUERY_INFORMATION |
            PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE;
        processAccess |= PROCESS_SET_INFORMATION;

        gameoptimizer::engine::UniqueHandle process(OpenProcess(
            processAccess, FALSE, processId));
        if (!process.IsValid())
        {
            gameoptimizer::engine::SetMessage(*startupStatus,
                gameoptimizer::engine::ErrorMessage(L"게임 프로세스에 접근할 수 없습니다"));
            return nullptr;
        }

        const std::wstring path = gameoptimizer::engine::ProcessImagePath(process.Get());
        if (requirePathVerification != 0 && path.empty())
        {
            gameoptimizer::engine::SetMessage(*startupStatus,
                gameoptimizer::engine::ErrorMessage(L"게임 실행 경로를 다시 확인하지 못했습니다"));
            return nullptr;
        }

        std::wstring error;
        if (!gameoptimizer::engine::ValidateTarget(
            process.Get(), processId, processCreationFileTime, path, error))
        {
            gameoptimizer::engine::SetMessage(*startupStatus, error);
            return nullptr;
        }

        auto session = std::make_unique<GO_SESSION>(std::move(process), processId,
            processCreationFileTime, path, journalPath,
            diagnosticsPath ? std::wstring(diagnosticsPath) : std::wstring{});
        session->impl.Start();
        session->impl.ReadStatus(*startupStatus);
        return session.release();
    }
    catch (...)
    {
        gameoptimizer::engine::SetMessage(*startupStatus, L"C++ 엔진 시작 중 오류가 발생했습니다.");
        return nullptr;
    }
}

extern "C" GO_API int GO_CALL go_read_status(GO_SESSION* session, GO_STATUS* status) noexcept // 세션 진행·복구 상태 C# UI 전달
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

extern "C" GO_API int GO_CALL go_request_stop(GO_SESSION* session) noexcept // 세션 작업자에 중단 요청 전달
{
    if (!session) return 0;
    session->impl.RequestStop();
    return 1;
}

extern "C" GO_API int GO_CALL go_wait(GO_SESSION* session, std::uint32_t timeoutMilliseconds) noexcept // 제한 시간 내 세션 작업자 완료 대기
{
    if (!session) return -1;
    return session->impl.Wait(timeoutMilliseconds);
}

extern "C" GO_API int GO_CALL go_try_restore(GO_SESSION* session) noexcept // 저장 CPU·우선순위 설정 원복 재시도
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

extern "C" GO_API int GO_CALL go_destroy_session(GO_SESSION* session) noexcept // 완료·복구 확인 후 세션 객체 해제
{
    if (!session) return 1;
    session->impl.RequestStop();
    session->impl.Join();
    if (session->impl.HasPendingRecovery()) return 0;
    delete session;
    return 1;
}

extern "C" GO_API int GO_CALL go_restore_journal(const wchar_t* journalPath) noexcept
{
    if (!journalPath || !*journalPath) return 0;
    try
    {
        std::wstring error;
        return gameoptimizer::engine::RestoreRecoveryJournal(journalPath, error) ? 1 : 0;
    }
    catch (...)
    {
        return 0;
    }
}
