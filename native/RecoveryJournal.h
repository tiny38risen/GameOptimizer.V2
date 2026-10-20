#pragma once

#include "EngineInternals.h"

#include <cstdint>
#include <string>

namespace gameoptimizer::engine
{
    // Each record is flushed before the corresponding Windows setting changes.
    bool RecordProcessAffinity(const std::wstring& path, DWORD processId,
        std::uint64_t creationTime, KAFFINITY original, KAFFINITY applied,
        std::wstring& error);
    bool RecordImplicitThreadAffinity(const std::wstring& path, DWORD processId,
        std::uint64_t processCreationTime, DWORD threadId,
        std::uint64_t threadCreationTime, const GROUP_AFFINITY& original,
        KAFFINITY originalProcessMask, KAFFINITY appliedProcessMask,
        std::wstring& error);
    bool RecordThreadAffinity(const std::wstring& path, std::uint64_t processCreationTime,
        const ThreadIdentity& identity,
        const GROUP_AFFINITY& original, const GROUP_AFFINITY& applied,
        std::wstring& error);
    bool RecordThreadPriority(const std::wstring& path, std::uint64_t processCreationTime,
        const ThreadIdentity& identity,
        int original, int applied, std::wstring& error);
    bool RecordProcessPriority(const std::wstring& path, DWORD processId,
        std::uint64_t creationTime, DWORD original, DWORD applied,
        std::wstring& error);
    bool RestoreRecoveryJournal(const std::wstring& path, std::wstring& error);
    void RemoveRecoveryJournal(const std::wstring& path) noexcept;
}
