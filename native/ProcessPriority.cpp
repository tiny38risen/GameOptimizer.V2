#include "ProcessPriority.h"
#include "RecoveryJournal.h"

namespace gameoptimizer::engine
{
    namespace
    {
        const wchar_t* PriorityClassName(DWORD priorityClass) noexcept // Windows 우선순위 등급명 변환
        {
            return priorityClass == HIGH_PRIORITY_CLASS ? L"High" : L"Unknown";
        }
    }

    bool ProcessPriorityController::Apply(HANDLE process, DWORD processId,
        std::uint64_t processCreationTime, const std::wstring& processPath,
        DWORD desiredPriorityClass, const std::wstring& journalPath,
        const std::atomic<bool>& stopRequested,
        std::wstring& message) // 게임 프로세스 우선순위 적용·검증
    {
        message.clear();
        if (desiredPriorityClass == 0)
            return true;

        if (desiredPriorityClass != HIGH_PRIORITY_CLASS)
        {
            message = L"지원하지 않는 프로세스 우선순위 값입니다.";
            return false;
        }
        if (pending_)
        {
            message = L"이전 프로세스 우선순위의 복구가 끝나지 않아 새 값을 적용하지 않았습니다.";
            return false;
        }
        if (stopRequested.load())
            return true;

        std::wstring error;
        if (!ValidateTarget(process, processId, processCreationTime, processPath, error))
        {
            message = error;
            return false;
        }

        const DWORD original = GetPriorityClass(process);
        if (original == 0)
        {
            message = ErrorMessage(L"게임 프로세스의 우선순위를 읽지 못했습니다");
            return false;
        }
        if (original == desiredPriorityClass)
        {
            message = L"프로세스 우선순위 등급이 이미 " +
                std::wstring(PriorityClassName(desiredPriorityClass)) + L"입니다.";
            return true;
        }

        if (stopRequested.load())
            return true;
        if (!ValidateTarget(process, processId, processCreationTime, processPath, error))
        {
            message = error;
            return false;
        }
        if (stopRequested.load())
            return true;

        if (!RecordProcessPriority(journalPath, processId, processCreationTime,
            original, desiredPriorityClass, message))
            return false;

        originalPriorityClass_ = original;
        appliedPriorityClass_ = desiredPriorityClass;
        pending_ = true;

        if (!SetPriorityClass(process, appliedPriorityClass_))
        {
            message = ErrorMessage(L"게임 프로세스 우선순위를 적용하지 못했습니다");
            if (!TryRestore(process, processId, processCreationTime, processPath))
                message += L" 복구 확인 필요: " + lastError_;
            return false;
        }

        if (GetPriorityClass(process) != appliedPriorityClass_)
        {
            message = L"프로세스 우선순위 적용 후 읽기 검증에 실패했습니다.";
            if (!TryRestore(process, processId, processCreationTime, processPath))
                message += L" 복구 확인 필요: " + lastError_;
            return false;
        }

        lastError_.clear();
        message = L"프로세스 우선순위를 " +
            std::wstring(PriorityClassName(desiredPriorityClass)) + L" 등급으로 적용하고 검증했습니다.";
        return true;
    }

    bool ProcessPriorityController::TryRestore(HANDLE process, DWORD processId,
        std::uint64_t processCreationTime, const std::wstring& processPath) // 저장된 게임 프로세스 우선순위 복구
    {
        if (!pending_)
        {
            lastError_.clear();
            return true;
        }

        const DWORD wait = WaitForSingleObject(process, 0);
        if (wait == WAIT_OBJECT_0)
        {
            pending_ = false;
            originalPriorityClass_ = 0;
            appliedPriorityClass_ = 0;
            lastError_.clear();
            return true;
        }
        if (wait != WAIT_TIMEOUT)
        {
            lastError_ = L"게임 프로세스의 생존 상태를 확인할 수 없습니다.";
            return false;
        }

        std::wstring error;
        if (!ValidateTarget(process, processId, processCreationTime, processPath, error))
        {
            lastError_ = error;
            return false;
        }

        const DWORD current = GetPriorityClass(process);
        if (current == 0)
        {
            lastError_ = ErrorMessage(L"복구 전 프로세스 우선순위를 읽지 못했습니다");
            return false;
        }
        if (current == originalPriorityClass_)
        {
            pending_ = false;
            originalPriorityClass_ = 0;
            appliedPriorityClass_ = 0;
            lastError_.clear();
            return true;
        }
        if (current != appliedPriorityClass_)
        {
            lastError_ = L"프로세스 우선순위가 외부에서 변경되어 안전한 원복을 확인할 수 없습니다.";
            return false;
        }
        if (!SetPriorityClass(process, originalPriorityClass_) ||
            GetPriorityClass(process) != originalPriorityClass_)
        {
            lastError_ = L"원래 프로세스 우선순위를 복구하거나 검증하지 못했습니다.";
            return false;
        }

        pending_ = false;
        originalPriorityClass_ = 0;
        appliedPriorityClass_ = 0;
        lastError_.clear();
        return true;
    }
}
