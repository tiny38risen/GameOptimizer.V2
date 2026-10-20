#pragma once

#include "EngineApi.h"

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace gameoptimizer::engine
{
    class UniqueHandle
    {
    public:
        explicit UniqueHandle(HANDLE handle = nullptr) noexcept : handle_(handle) {} // Windows 핸들 RAII 보관
        ~UniqueHandle() { Reset(); } // 소유 핸들 자동 닫기

        UniqueHandle(const UniqueHandle&) = delete; // 핸들 이중 소유 방지용 복사 금지
        UniqueHandle& operator=(const UniqueHandle&) = delete; // 핸들 이중 소유 방지용 복사 대입 금지

        UniqueHandle(UniqueHandle&& other) noexcept : handle_(other.Release()) {} // 핸들 소유권 이동
        UniqueHandle& operator=(UniqueHandle&& other) noexcept // 기존 핸들 닫기·새 소유권 이전
        {
            if (this != &other)
            {
                Reset();
                handle_ = other.Release();
            }
            return *this;
        }

        HANDLE Get() const noexcept { return handle_; } // 보관 핸들 반환
        bool IsValid() const noexcept { return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE; } // 핸들 유효성 확인

        HANDLE Release() noexcept // 핸들 소유권 반환
        {
            HANDLE result = handle_;
            handle_ = nullptr;
            return result;
        }

        void Reset(HANDLE handle = nullptr) noexcept // 기존 핸들 닫기·새 핸들 교체
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

        bool operator==(const ThreadIdentity& other) const noexcept // PID·TID·생성 시각 비교
        {
            return ProcessId == other.ProcessId && ThreadId == other.ThreadId &&
                CreationFileTime == other.CreationFileTime;
        }
    };

    struct ThreadIdentityHash
    {
        std::size_t operator()(const ThreadIdentity& identity) const noexcept // 스레드 식별 해시 키 생성
        {
            const auto processHash = std::hash<DWORD>{}(identity.ProcessId);
            const auto threadHash = std::hash<DWORD>{}(identity.ThreadId);
            const auto creationHash = std::hash<std::uint64_t>{}(identity.CreationFileTime);
            return processHash ^ (threadHash << 1) ^ (creationHash << 2);
        }
    };

    inline std::uint64_t FileTimeValue(const FILETIME& value) noexcept // FILETIME 64비트 값 변환
    {
        ULARGE_INTEGER result{};
        result.LowPart = value.dwLowDateTime;
        result.HighPart = value.dwHighDateTime;
        return result.QuadPart;
    }

    inline std::wstring ErrorMessage(const wchar_t* message, DWORD error = GetLastError()) // Windows 오류 메시지 구성
    {
        return std::wstring(message) + L" (" + std::to_wstring(error) + L").";
    }

    inline std::wstring ProcessImagePath(HANDLE process) // 프로세스 실행 경로 조회
    {
        std::vector<wchar_t> path(32768);
        DWORD length = static_cast<DWORD>(path.size());
        if (!QueryFullProcessImageNameW(process, 0, path.data(), &length)) return {};
        return std::wstring(path.data(), length);
    }

    inline void SetMessage(GO_STATUS& status, const std::wstring& message) noexcept // C ABI 상태 버퍼에 메시지 복사
    {
        wcsncpy_s(status.Message, _countof(status.Message), message.c_str(), _TRUNCATE);
    }

    inline void InitializeStatus(GO_STATUS& status, bool passed, const std::wstring& message) noexcept // 상태 구조체 초기화·안내 문구 설정
    {
        status = {};
        status.Passed = passed ? 1U : 0U;
        SetMessage(status, message);
    }

    inline bool ValidateTarget(
        HANDLE process,
        DWORD processId,
        std::uint64_t expectedCreationTime,
        const std::wstring& expectedPath,
        std::wstring& error) // PID·생성 시각·실행 경로 검증
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
            const std::wstring actualPath = ProcessImagePath(process);
            if (actualPath.empty())
            {
                error = ErrorMessage(L"게임 실행 경로를 다시 확인하지 못했습니다");
                return false;
            }

            if (CompareStringOrdinal(actualPath.c_str(), static_cast<int>(actualPath.size()),
                expectedPath.c_str(), static_cast<int>(expectedPath.size()), TRUE) != CSTR_EQUAL)
            {
                error = L"선택한 PID의 실행 파일 경로가 달라졌습니다.";
                return false;
            }
        }

        return true;
    }
}
