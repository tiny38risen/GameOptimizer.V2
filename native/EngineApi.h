#pragma once

#include <cstdint>

#if defined(GAMEOPTIMIZER_ENGINE_EXPORTS)
#define GO_API __declspec(dllexport)
#else
#define GO_API __declspec(dllimport)
#endif

#define GO_CALL __cdecl

struct GO_SESSION;

constexpr std::uint32_t GO_WINDOW_TITLE_CAPACITY = 512;
constexpr std::uint32_t GO_EXECUTABLE_CAPACITY = 260;

struct GO_GAME_TARGET
{
    std::uint32_t ProcessId;
    std::uint64_t ProcessCreationFileTime;
    std::uint32_t PathAvailable;
    wchar_t WindowTitle[GO_WINDOW_TITLE_CAPACITY];
    wchar_t Executable[GO_EXECUTABLE_CAPACITY];
};

struct GO_STATUS
{
    std::uint32_t HasCandidate;
    std::uint32_t Confirmed;
    std::uint32_t Applied;
    std::uint32_t RecoveryRequired;
    std::uint32_t Passed;
    std::uint32_t ProcessId;
    std::uint32_t ThreadId;
    wchar_t Message[512];
};

extern "C"
{
    GO_API int GO_CALL go_list_visible_games(
        GO_GAME_TARGET* targets,
        std::uint32_t capacity,
        std::uint32_t* requiredCount) noexcept; // 보이는 앱 대상 목록 반환

    GO_API GO_SESSION* GO_CALL go_start_session(
        std::uint32_t processId,
        std::uint64_t processCreationFileTime,
        std::uint32_t requirePathVerification,
        const wchar_t* journalPath,
        const wchar_t* diagnosticsPath, // nullptr이면 진단 로그 생략
        GO_STATUS* startupStatus) noexcept; // PID·생성 시각·경로 검증 및 네이티브 세션 시작

    GO_API int GO_CALL go_restore_journal(const wchar_t* journalPath) noexcept;

    GO_API int GO_CALL go_read_status(GO_SESSION* session, GO_STATUS* status) noexcept; // 진행·적용·복구 상태 조회
    GO_API int GO_CALL go_request_stop(GO_SESSION* session) noexcept; // 세션 중단·원복 요청
    GO_API int GO_CALL go_wait(GO_SESSION* session, std::uint32_t timeoutMilliseconds) noexcept; // 제한 시간 내 세션 완료 대기
    GO_API int GO_CALL go_try_restore(GO_SESSION* session) noexcept; // 저장 설정 원복 재시도
    GO_API int GO_CALL go_destroy_session(GO_SESSION* session) noexcept; // 복구 완료 세션 객체 해제
}
