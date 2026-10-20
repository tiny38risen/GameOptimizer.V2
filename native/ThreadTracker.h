#pragma once

#include "EngineInternals.h"

#include <memory>
#include <string>

namespace gameoptimizer::engine
{
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
        ThreadCandidateTracker(); // 스레드 CPU 추적기 초기화
        ~ThreadCandidateTracker(); // 내부 추적 자원 해제

        ThreadCandidateTracker(const ThreadCandidateTracker&) = delete; // CPU 표본 이력 보호용 복사 금지
        ThreadCandidateTracker& operator=(const ThreadCandidateTracker&) = delete; // CPU 표본 이력 보호용 복사 대입 금지

        CandidateObservation Observe(DWORD processId); // 프로세스 CPU 우세 스레드 후보 관찰

    private:
        class Impl;
        std::unique_ptr<Impl> impl_;
    };
}
