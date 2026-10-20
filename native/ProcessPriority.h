#pragma once

#include "EngineInternals.h"

#include <atomic>
#include <string>

namespace gameoptimizer::engine
{
    class ProcessPriorityController
    {
    public:
        bool HasPendingRecovery() const noexcept { return pending_; } // 프로세스 우선순위 원복 여부 확인
        const std::wstring& LastError() const noexcept { return lastError_; } // 최근 원복 오류 조회

        bool Apply(HANDLE process, DWORD processId, std::uint64_t processCreationTime,
            const std::wstring& processPath, DWORD desiredPriorityClass,
            const std::wstring& journalPath,
            const std::atomic<bool>& stopRequested, std::wstring& message); // 프로세스 우선순위 적용·검증
        bool TryRestore(HANDLE process, DWORD processId,
            std::uint64_t processCreationTime, const std::wstring& processPath); // 저장된 프로세스 우선순위 복구

    private:
        bool pending_ = false;
        DWORD originalPriorityClass_ = 0;
        DWORD appliedPriorityClass_ = 0;
        std::wstring lastError_;
    };
}
