#pragma once

#include "EngineResult.h"

#include <atomic>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace gameoptimizer::engine
{
    struct CpuCoreCandidate
    {
        std::uint32_t PhysicalCoreIndex = 0;
        std::uint32_t LogicalProcessorIndex = 0;
        GROUP_AFFINITY LogicalProcessor{};
        std::uint32_t L3Index = 0;
        double RelativeSpeedPercent = 0.0;
        double InterruptDpcPercent = 0.0;
        std::optional<std::uint8_t> EfficiencyClass;
    };

    struct CpuPlan
    {
        GROUP_AFFINITY Game{};
        GROUP_AFFINITY Dominant{};
        GROUP_AFFINITY Workers{};
        GROUP_AFFINITY Reserved{};
        CpuCoreCandidate DominantCore{};
        CpuCoreCandidate WorkerCore{};
    };

    struct AffinityApplyResult
    {
        std::size_t RestrictedCount = 0;
        std::vector<std::wstring> RestrictedSamples;
    };

    class ProcessAffinityController
    {
    public:
        ProcessAffinityController() = default; // 프로세스 CPU 범위·원복 컨트롤러 생성
        ProcessAffinityController(const ProcessAffinityController&) = delete; // 원복 정보 복제 방지용 복사 금지
        ProcessAffinityController& operator=(const ProcessAffinityController&) = delete; // 원복 정보 보호용 복사 대입 금지

        bool HasPendingRecovery() const noexcept; // 저장 affinity 원복 여부 확인
        const std::wstring& LastError() const noexcept { return lastError_; } // 최근 프로세스 affinity 원복 오류 조회

        [[nodiscard]] Result<AffinityApplyResult> Apply(HANDLE gameProcess, DWORD gameProcessId,
            std::uint64_t gameCreationTime, const std::wstring& gamePath,
            const CpuPlan& plan, const std::wstring& journalPath,
            const std::atomic<bool>& stopRequested); // 게임·나머지 프로세스 CPU 범위 적용·검증
        bool TryRestore(); // 저장된 프로세스·스레드 CPU 범위 복구

    private:
        struct PendingThreadRestore
        {
            UniqueHandle Thread;
            DWORD ThreadId = 0;
            std::uint64_t CreationFileTime = 0;
            GROUP_AFFINITY OriginalAffinity{};

            PendingThreadRestore(UniqueHandle&& thread, DWORD threadId,
                std::uint64_t creationFileTime,
                const GROUP_AFFINITY& originalAffinity) noexcept // 스레드 핸들·원래 CPU 범위 저장
                : Thread(std::move(thread)), ThreadId(threadId),
                  CreationFileTime(creationFileTime), OriginalAffinity(originalAffinity)
            {
            }
        };

        struct PendingRestore
        {
            UniqueHandle Process;
            DWORD ProcessId = 0;
            std::uint64_t CreationFileTime = 0;
            KAFFINITY OriginalMask = 0;
            KAFFINITY AppliedMask = 0;
            bool NeedsRestore = false;
            std::vector<PendingThreadRestore> Threads;

            PendingRestore(UniqueHandle&& process, DWORD processId,
                std::uint64_t creationFileTime, KAFFINITY originalMask,
                KAFFINITY appliedMask,
                std::vector<PendingThreadRestore>&& threads) noexcept // 프로세스·기존 CPU 범위·스레드 복구 정보 저장
                : Process(std::move(process)), ProcessId(processId),
                  CreationFileTime(creationFileTime), OriginalMask(originalMask),
                  AppliedMask(appliedMask), NeedsRestore(originalMask != appliedMask),
                  Threads(std::move(threads))
            {
            }
        };

        bool CaptureThreadAffinities(DWORD processId, KAFFINITY processMask,
            std::vector<PendingThreadRestore>& threads, DWORD& errorCode,
            std::wstring& error); // 프로세스 범위 변경 전 스레드 값 저장

        std::vector<PendingRestore> pending_;
        std::wstring lastError_;
    };
}
