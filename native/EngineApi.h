#pragma once

#include <cstdint>

#if defined(GAMEOPTIMIZER_ENGINE_EXPORTS)
#define GO_API __declspec(dllexport)
#else
#define GO_API __declspec(dllimport)
#endif

#define GO_CALL __cdecl

struct GO_SESSION;

struct GO_STATUS
{
    std::uint32_t HasCandidate;
    std::uint32_t Confirmed;
    std::uint32_t RecoveryRequired;
    std::uint32_t Passed;
    std::uint32_t ProcessId;
    std::uint32_t ThreadId;
    wchar_t Message[512];
};

extern "C"
{
    GO_API GO_SESSION* GO_CALL go_start_session(
        std::uint32_t processId,
        std::uint64_t processCreationFileTime,
        const wchar_t* fullPath,
        GO_STATUS* startupStatus) noexcept;

    GO_API int GO_CALL go_read_status(GO_SESSION* session, GO_STATUS* status) noexcept;
    GO_API int GO_CALL go_request_stop(GO_SESSION* session) noexcept;
    GO_API int GO_CALL go_wait(GO_SESSION* session, std::uint32_t timeoutMilliseconds) noexcept;
    GO_API int GO_CALL go_try_restore(GO_SESSION* session) noexcept;
    GO_API int GO_CALL go_destroy_session(GO_SESSION* session) noexcept;
}
