#pragma once

#include "EngineInternals.h"
#include "ProcessAffinityController.h"

#include <atomic>
#include <memory>
#include <string>

namespace gameoptimizer::engine
{
    struct ApplyResult
    {
        bool Success = false;
        std::wstring Message;
    };

    class ThreadPolicyController
    {
    public:
        ThreadPolicyController(); // 스레드 정책 컨트롤러 생성
        ~ThreadPolicyController(); // 내부 스레드 정책 상태 해제

        ThreadPolicyController(const ThreadPolicyController&) = delete; // 저장된 스레드 원복 정보 복제 방지용 복사 금지
        ThreadPolicyController& operator=(const ThreadPolicyController&) = delete; // 저장된 스레드 원복 정보 보호용 복사 대입 금지

        bool HasPendingRecovery() const noexcept; // 복구 필요 스레드 설정 여부 확인
        const std::wstring& LastError() const noexcept; // 최근 스레드 정책 오류 조회
        ApplyResult Apply(HANDLE process, DWORD processId,
            std::uint64_t processCreationTime, const std::wstring& processPath,
            const CpuPlan& plan, const ThreadIdentity& dominantThread,
            const std::wstring& journalPath,
            const std::atomic<bool>& stopRequested); // 지배 스레드·나머지 스레드 CPU 배치 적용
        bool TryRestore(); // 저장된 게임 스레드 설정 복구

    private:
        class Impl;
        std::unique_ptr<Impl> impl_;
    };
}
