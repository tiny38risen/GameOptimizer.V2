#include "RecoveryJournal.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace gameoptimizer::engine
{
    namespace
    {
        struct Record
        {
            char Kind = 0; // T: thread affinity, U: thread priority, A: process affinity,
                           // H: affinity inherited from a process change, P: process priority.
            DWORD ProcessId = 0;
            std::uint64_t ProcessCreation = 0;
            DWORD ThreadId = 0;
            std::uint64_t ThreadCreation = 0;
            WORD OriginalGroup = 0;
            std::uint64_t OriginalMask = 0;
            WORD AppliedGroup = 0;
            std::uint64_t AppliedMask = 0;
            int OriginalPriority = 0;
            int AppliedPriority = 0;
            std::uint64_t AlternativeMask = 0;
        };

        bool Append(const std::wstring& path, const Record& record, std::wstring& error)
        {
            if (path.empty())
            {
                error = L"복구 기록 경로가 없습니다.";
                return false;
            }

            std::ostringstream line;
            line << record.Kind << ' ' << record.ProcessId << ' ' << record.ProcessCreation
                << ' ' << record.ThreadId << ' ' << record.ThreadCreation
                << ' ' << record.OriginalGroup << ' ' << record.OriginalMask
                << ' ' << record.AppliedGroup << ' ' << record.AppliedMask
                << ' ' << record.OriginalPriority << ' ' << record.AppliedPriority
                << ' ' << record.AlternativeMask << '\n';
            const std::string bytes = line.str();
            UniqueHandle file(CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr));
            if (!file.IsValid())
            {
                error = ErrorMessage(L"복구 기록을 열지 못했습니다");
                return false;
            }

            DWORD written = 0;
            if (!WriteFile(file.Get(), bytes.data(), static_cast<DWORD>(bytes.size()),
                    &written, nullptr) || written != bytes.size() ||
                !FlushFileBuffers(file.Get()))
            {
                error = ErrorMessage(L"복구 기록을 저장하지 못했습니다");
                return false;
            }
            return true;
        }

        bool SameProcess(HANDLE process, const Record& record)
        {
            if (GetProcessId(process) != record.ProcessId) return false;
            FILETIME creation{}, exit{}, kernel{}, user{};
            return GetProcessTimes(process, &creation, &exit, &kernel, &user) &&
                FileTimeValue(creation) == record.ProcessCreation;
        }

        bool SameThread(HANDLE thread, const Record& record)
        {
            if (GetThreadId(thread) != record.ThreadId ||
                GetProcessIdOfThread(thread) != record.ProcessId)
                return false;
            FILETIME creation{}, exit{}, kernel{}, user{};
            return GetThreadTimes(thread, &creation, &exit, &kernel, &user) &&
                FileTimeValue(creation) == record.ThreadCreation;
        }

        bool SameAffinity(const GROUP_AFFINITY& current, WORD group,
            std::uint64_t mask) noexcept
        {
            return current.Group == group && current.Mask == mask;
        }

        bool RestoreOne(const Record& record, std::wstring& error)
        {
            UniqueHandle process(OpenProcess(PROCESS_QUERY_INFORMATION |
                PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SET_INFORMATION | SYNCHRONIZE,
                FALSE, record.ProcessId));
            if (!process.IsValid())
            {
                if (GetLastError() == ERROR_INVALID_PARAMETER) return true; // Process exited.
                error = ErrorMessage(L"복구 대상 프로세스를 열지 못했습니다");
                return false;
            }
            if (!SameProcess(process.Get(), record)) return true; // PID was reused.
            const DWORD processWait = WaitForSingleObject(process.Get(), 0);
            if (processWait == WAIT_OBJECT_0) return true;
            if (processWait != WAIT_TIMEOUT)
            {
                error = L"복구 대상 프로세스의 생존 상태를 확인하지 못했습니다.";
                return false;
            }

            if (record.Kind == 'A')
            {
                KAFFINITY current = 0, system = 0;
                if (!GetProcessAffinityMask(process.Get(), &current, &system))
                {
                    error = ErrorMessage(L"복구 전 프로세스 CPU 범위를 읽지 못했습니다");
                    return false;
                }
                if (current == record.OriginalMask) return true;
                if (current != record.AppliedMask ||
                    !SetProcessAffinityMask(process.Get(), record.OriginalMask) ||
                    !GetProcessAffinityMask(process.Get(), &current, &system) ||
                    current != record.OriginalMask)
                {
                    error = L"프로세스 CPU 범위를 안전하게 원복하지 못했습니다.";
                    return false;
                }
                return true;
            }
            if (record.Kind == 'P')
            {
                const DWORD current = GetPriorityClass(process.Get());
                if (current == static_cast<DWORD>(record.OriginalPriority)) return true;
                if (current != static_cast<DWORD>(record.AppliedPriority) ||
                    !SetPriorityClass(process.Get(), static_cast<DWORD>(record.OriginalPriority)) ||
                    GetPriorityClass(process.Get()) != static_cast<DWORD>(record.OriginalPriority))
                {
                    error = L"프로세스 우선순위를 안전하게 원복하지 못했습니다.";
                    return false;
                }
                return true;
            }

            UniqueHandle thread(OpenThread(THREAD_QUERY_INFORMATION |
                THREAD_SET_INFORMATION | SYNCHRONIZE, FALSE, record.ThreadId));
            if (!thread.IsValid())
            {
                if (GetLastError() == ERROR_INVALID_PARAMETER) return true; // Thread exited.
                error = ErrorMessage(L"복구 대상 스레드를 열지 못했습니다");
                return false;
            }
            if (!SameThread(thread.Get(), record)) return true; // TID was reused.
            const DWORD threadWait = WaitForSingleObject(thread.Get(), 0);
            if (threadWait == WAIT_OBJECT_0) return true;
            if (threadWait != WAIT_TIMEOUT)
            {
                error = L"복구 대상 스레드의 생존 상태를 확인하지 못했습니다.";
                return false;
            }

            if (record.Kind == 'U')
            {
                SetLastError(ERROR_SUCCESS);
                const int current = GetThreadPriority(thread.Get());
                if (current == THREAD_PRIORITY_ERROR_RETURN && GetLastError() != ERROR_SUCCESS)
                {
                    error = ErrorMessage(L"복구 전 스레드 우선순위를 읽지 못했습니다");
                    return false;
                }
                if (current == record.OriginalPriority) return true;
                if (current != record.AppliedPriority ||
                    !SetThreadPriority(thread.Get(), record.OriginalPriority) ||
                    GetThreadPriority(thread.Get()) != record.OriginalPriority)
                {
                    error = L"스레드 우선순위를 안전하게 원복하지 못했습니다.";
                    return false;
                }
                return true;
            }

            GROUP_AFFINITY current{};
            if (!GetThreadGroupAffinity(thread.Get(), &current))
            {
                error = ErrorMessage(L"복구 전 스레드 CPU 범위를 읽지 못했습니다");
                return false;
            }
            if (SameAffinity(current, record.OriginalGroup, record.OriginalMask))
                return true;
            if (!SameAffinity(current, record.AppliedGroup, record.AppliedMask) &&
                !(record.Kind == 'H' && SameAffinity(current, 0, record.AlternativeMask)))
            {
                error = L"스레드 CPU 범위가 다른 프로그램에 의해 변경되었습니다.";
                return false;
            }
            GROUP_AFFINITY original{};
            original.Group = record.OriginalGroup;
            original.Mask = record.OriginalMask;
            if (!SetThreadGroupAffinity(thread.Get(), &original, nullptr) ||
                !GetThreadGroupAffinity(thread.Get(), &current) ||
                !SameAffinity(current, original.Group, original.Mask))
            {
                error = L"스레드 CPU 범위를 안전하게 원복하지 못했습니다.";
                return false;
            }
            return true;
        }
    }

    bool RecordProcessAffinity(const std::wstring& path, DWORD processId,
        std::uint64_t creationTime, KAFFINITY original, KAFFINITY applied,
        std::wstring& error)
    {
        return Append(path, { 'A', processId, creationTime, 0, 0, 0,
            original, 0, applied, 0, 0, 0 }, error);
    }

    bool RecordImplicitThreadAffinity(const std::wstring& path, DWORD processId,
        std::uint64_t processCreationTime, DWORD threadId,
        std::uint64_t threadCreationTime, const GROUP_AFFINITY& original,
        KAFFINITY originalProcessMask, KAFFINITY appliedProcessMask,
        std::wstring& error)
    {
        return Append(path, { 'H', processId, processCreationTime, threadId,
            threadCreationTime, original.Group, original.Mask, 0,
            appliedProcessMask, 0, 0, originalProcessMask }, error);
    }

    bool RecordThreadAffinity(const std::wstring& path, std::uint64_t processCreationTime,
        const ThreadIdentity& identity,
        const GROUP_AFFINITY& original, const GROUP_AFFINITY& applied,
        std::wstring& error)
    {
        return Append(path, { 'T', identity.ProcessId, processCreationTime, identity.ThreadId,
            identity.CreationFileTime, original.Group, original.Mask,
            applied.Group, applied.Mask, 0, 0, 0 }, error);
    }

    bool RecordThreadPriority(const std::wstring& path, std::uint64_t processCreationTime,
        const ThreadIdentity& identity,
        int original, int applied, std::wstring& error)
    {
        return Append(path, { 'U', identity.ProcessId, processCreationTime, identity.ThreadId,
            identity.CreationFileTime, 0, 0, 0, 0, original, applied, 0 }, error);
    }

    bool RecordProcessPriority(const std::wstring& path, DWORD processId,
        std::uint64_t creationTime, DWORD original, DWORD applied,
        std::wstring& error)
    {
        return Append(path, { 'P', processId, creationTime, 0, 0, 0, 0, 0, 0,
            static_cast<int>(original), static_cast<int>(applied), 0 }, error);
    }

    bool RestoreRecoveryJournal(const std::wstring& path, std::wstring& error)
    {
        error.clear();
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
            return GetLastError() == ERROR_FILE_NOT_FOUND;

        std::ifstream input{ std::filesystem::path(path) };
        if (!input)
        {
            error = L"복구 기록을 읽지 못했습니다.";
            return false;
        }

        std::vector<Record> records;
        std::string line;
        while (std::getline(input, line))
        {
            // A crash during append can leave an incomplete final line. No setting
            // is changed until that write and flush succeed, so ignore the tail.
            if (input.eof()) break;
            Record record{};
            std::istringstream values(line);
            if (!(values >> record.Kind >> record.ProcessId >> record.ProcessCreation
                    >> record.ThreadId >> record.ThreadCreation >> record.OriginalGroup
                    >> record.OriginalMask >> record.AppliedGroup >> record.AppliedMask
                    >> record.OriginalPriority >> record.AppliedPriority
                    >> record.AlternativeMask) ||
                (record.Kind != 'T' && record.Kind != 'U' && record.Kind != 'A' &&
                 record.Kind != 'H' && record.Kind != 'P'))
            {
                error = L"복구 기록 형식이 올바르지 않습니다.";
                return false;
            }
            records.push_back(record);
        }
        if (input.bad())
        {
            error = L"복구 기록을 끝까지 읽지 못했습니다.";
            return false;
        }
        input.close();

        bool restored = true;
        for (const char kind : std::array{ 'T', 'U', 'A', 'H', 'P' })
        {
            for (auto item = records.rbegin(); item != records.rend(); ++item)
            {
                if (item->Kind != kind) continue;
                std::wstring itemError;
                if (!RestoreOne(*item, itemError))
                {
                    restored = false;
                    if (error.empty()) error = std::move(itemError);
                }
            }
        }
        if (!restored) return false;
        if (!DeleteFileW(path.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND)
        {
            error = ErrorMessage(L"복구 기록을 정리하지 못했습니다");
            return false;
        }
        return true;
    }

    void RemoveRecoveryJournal(const std::wstring& path) noexcept
    {
        if (!path.empty()) DeleteFileW(path.c_str());
    }
}
