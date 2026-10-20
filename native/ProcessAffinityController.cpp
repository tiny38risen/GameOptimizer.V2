#include "ProcessAffinityController.h"
#include "RecoveryJournal.h"

#include <Pdh.h>
#include <PdhMsg.h>
#include <TlHelp32.h>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <iterator>
#include <vector>

namespace gameoptimizer::engine
{
    namespace
    {
        bool ReadProcessIdentity(HANDLE process, DWORD expectedProcessId,
            std::uint64_t& creationTime) noexcept // PID·생성 시각 기반 프로세스 식별
        {
            if (GetProcessId(process) != expectedProcessId)
                return false;

            FILETIME creation{}, exit{}, kernel{}, user{};
            if (!GetProcessTimes(process, &creation, &exit, &kernel, &user))
                return false;

            creationTime = FileTimeValue(creation);
            return true;
        }

        bool IsCriticalProcess(HANDLE process) noexcept // Windows 중요 프로세스 확인
        {
            BOOL critical = FALSE;
            return IsProcessCritical(process, &critical) && critical != FALSE;
        }

        UniqueHandle DuplicateProcess(HANDLE process) // 복구용 게임 프로세스 핸들 복제
        {
            HANDLE duplicate = nullptr;
            if (!DuplicateHandle(GetCurrentProcess(), process, GetCurrentProcess(),
                &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS))
                return UniqueHandle{};
            return UniqueHandle(duplicate);
        }

        UniqueHandle OpenProcessForAffinity(DWORD processId, HANDLE gameProcess,
            DWORD gameProcessId) // CPU 범위 변경용 프로세스 핸들 열기
        {
            if (processId == gameProcessId)
                return DuplicateProcess(gameProcess);

            return UniqueHandle(OpenProcess(
                PROCESS_QUERY_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION |
                    PROCESS_SET_INFORMATION | SYNCHRONIZE,
                FALSE, processId));
        }

        const GROUP_AFFINITY* FindCoreGroupMask(
            const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX& information) noexcept // 물리 코어의 프로세서 그룹 마스크 조회
        {
            if (information.Relationship != RelationProcessorCore)
                return nullptr;

            for (WORD index = 0; index < information.Processor.GroupCount; ++index)
            {
                if (information.Processor.GroupMask[index].Group == 0)
                    return &information.Processor.GroupMask[index];
            }
            return nullptr;
        }

        bool SameProcessIdentity(HANDLE process, DWORD processId,
            std::uint64_t expectedCreationTime) noexcept // 저장 프로세스의 PID·생성 시각 확인
        {
            std::uint64_t creationTime = 0;
            return ReadProcessIdentity(process, processId, creationTime) &&
                creationTime == expectedCreationTime;
        }

        bool SameAffinity(const GROUP_AFFINITY& left,
            const GROUP_AFFINITY& right) noexcept // 스레드 그룹·마스크 비교
        {
            return left.Group == right.Group && left.Mask == right.Mask;
        }

        bool MatchesThreadIdentity(HANDLE thread, DWORD processId, DWORD threadId,
            std::uint64_t expectedCreationTime) noexcept // 복구 스레드 PID·TID·생성 시각 확인
        {
            if (GetThreadId(thread) != threadId || GetProcessIdOfThread(thread) != processId)
                return false;

            FILETIME creation{}, exit{}, kernel{}, user{};
            return GetThreadTimes(thread, &creation, &exit, &kernel, &user) &&
                FileTimeValue(creation) == expectedCreationTime;
        }

        struct PdhQueryHandle
        {
            PDH_HQUERY Value = nullptr;
            ~PdhQueryHandle() { if (Value) PdhCloseQuery(Value); }
        };

        bool ReadLogicalProcessorUsage(std::vector<double>& usage, std::wstring& error) // 논리 프로세서별 평균 사용률 표본
        {
            const DWORD processorCount = GetMaximumProcessorCount(0);
            if (processorCount == 0)
            {
                error = L"논리 프로세서 수를 읽지 못했습니다.";
                return false;
            }

            PdhQueryHandle query;
            PDH_HCOUNTER counter = nullptr;
            PDH_STATUS status = PdhOpenQueryW(nullptr, 0, &query.Value);
            if (status != ERROR_SUCCESS)
            {
                error = L"CPU 사용률 측정을 시작하지 못했습니다 (PDH " + std::to_wstring(status) + L").";
                return false;
            }

            status = PdhAddEnglishCounterW(query.Value,
                L"\\Processor Information(*)\\% Processor Time", 0, &counter);
            if (status != ERROR_SUCCESS || PdhCollectQueryData(query.Value) != ERROR_SUCCESS)
            {
                error = L"논리 프로세서 사용률 카운터를 읽지 못했습니다 (PDH " + std::to_wstring(status) + L").";
                return false;
            }

            constexpr DWORD SampleIntervalMilliseconds = 500;
            constexpr std::size_t SampleCount = 6;
            std::vector<double> totals(processorCount, 0.0);
            std::vector<unsigned int> counts(processorCount, 0);

            for (std::size_t sample = 0; sample < SampleCount; ++sample)
            {
                Sleep(SampleIntervalMilliseconds);
                status = PdhCollectQueryData(query.Value);
                if (status != ERROR_SUCCESS)
                {
                    error = L"CPU 사용률 표본을 수집하지 못했습니다 (PDH " + std::to_wstring(status) + L").";
                    return false;
                }

                DWORD bufferSize = 0;
                DWORD itemCount = 0;
                status = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE,
                    &bufferSize, &itemCount, nullptr);
                if (status != PDH_MORE_DATA || bufferSize == 0)
                {
                    error = L"논리 프로세서별 CPU 사용률 목록을 읽지 못했습니다 (PDH " + std::to_wstring(status) + L").";
                    return false;
                }

                std::vector<std::byte> buffer(bufferSize);
                auto* items = reinterpret_cast<PPDH_FMT_COUNTERVALUE_ITEM_W>(buffer.data());
                status = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE,
                    &bufferSize, &itemCount, items);
                if (status != ERROR_SUCCESS)
                {
                    error = L"논리 프로세서별 CPU 사용률 값을 읽지 못했습니다 (PDH " + std::to_wstring(status) + L").";
                    return false;
                }

                for (DWORD index = 0; index < itemCount; ++index)
                {
                    unsigned int group = 0;
                    unsigned int processor = 0;
                    if (items[index].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA &&
                        items[index].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA)
                        continue;
                    if (swscanf_s(items[index].szName, L"%u,%u", &group, &processor) != 2 ||
                        group != 0 || processor >= processorCount)
                        continue;

                    const double value = items[index].FmtValue.doubleValue;
                    if (value < 0.0 || value > 100.0) continue;
                    totals[processor] += value;
                    ++counts[processor];
                }
            }

            usage.assign(processorCount, -1.0);
            for (DWORD processor = 0; processor < processorCount; ++processor)
            {
                if (counts[processor] != 0)
                    usage[processor] = totals[processor] / counts[processor];
            }
            return true;
        }

        std::vector<int> ReadEfficiencyClasses(DWORD processorCount) // 논리 CPU별 Windows 효율 등급 조회
        {
            ULONG requiredSize = 0;
            if (GetSystemCpuSetInformation(nullptr, 0, &requiredSize, nullptr, 0) ||
                GetLastError() != ERROR_INSUFFICIENT_BUFFER || requiredSize == 0)
                return {};

            std::vector<std::byte> buffer(requiredSize);
            if (!GetSystemCpuSetInformation(
                reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buffer.data()),
                requiredSize, &requiredSize, nullptr, 0))
                return {};

            std::vector<int> classes(processorCount, -1);
            std::size_t offset = 0;
            while (offset < requiredSize)
            {
                if (requiredSize - offset < sizeof(SYSTEM_CPU_SET_INFORMATION))
                    return {};

                const auto* information = reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(
                    buffer.data() + offset);
                if (information->Size < sizeof(SYSTEM_CPU_SET_INFORMATION) ||
                    information->Size > requiredSize - offset)
                    return {};

                if (information->Type == CpuSetInformation &&
                    information->CpuSet.Group == 0 &&
                    information->CpuSet.LogicalProcessorIndex < processorCount)
                {
                    classes[information->CpuSet.LogicalProcessorIndex] =
                        information->CpuSet.EfficiencyClass;
                }

                offset += information->Size;
            }
            return classes;
        }
    }

    std::optional<std::vector<CpuCoreCandidate>> FindCpuCoreCandidates(
        HANDLE gameProcess, std::wstring& error) // 사용률 상위 절반 제외 후보 조회
    {
        if (GetActiveProcessorGroupCount() != 1)
        {
            error = L"이 PC의 CPU 그룹 구성을 자동 분석할 수 없습니다.";
            return std::nullopt;
        }

        KAFFINITY gameMask = 0;
        KAFFINITY systemMask = 0;
        if (!GetProcessAffinityMask(gameProcess, &gameMask, &systemMask))
        {
            error = ErrorMessage(L"CPU 목록을 읽지 못했습니다");
            return std::nullopt;
        }

        DWORD bufferSize = 0;
        GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &bufferSize);
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bufferSize == 0)
        {
            error = ErrorMessage(L"물리 코어 구성을 읽지 못했습니다");
            return std::nullopt;
        }

        std::vector<std::byte> buffer(bufferSize);
        if (!GetLogicalProcessorInformationEx(RelationProcessorCore,
            reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()),
            &bufferSize))
        {
            error = ErrorMessage(L"물리 코어 구성을 읽지 못했습니다");
            return std::nullopt;
        }

        std::vector<double> processorUsage;
        if (!ReadLogicalProcessorUsage(processorUsage, error))
            return std::nullopt;
        const std::vector<int> efficiencyClasses = ReadEfficiencyClasses(
            static_cast<DWORD>(processorUsage.size()));

        std::vector<CpuCoreCandidate> candidates;
        std::uint32_t physicalCoreIndex = 0;
        std::size_t offset = 0;
        constexpr std::size_t coreGroupMaskOffset =
            offsetof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, Processor.GroupMask);
        while (offset < bufferSize)
        {
            if (bufferSize - offset < coreGroupMaskOffset)
            {
                error = L"물리 코어 구성 데이터가 올바르지 않습니다.";
                return std::nullopt;
            }

            const auto* information = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(
                buffer.data() + offset);
            const std::size_t recordSize = information->Size;
            if (information->Relationship != RelationProcessorCore ||
                recordSize < coreGroupMaskOffset || recordSize > bufferSize - offset)
            {
                error = L"물리 코어 구성 데이터가 올바르지 않습니다.";
                return std::nullopt;
            }

            const std::size_t groupCount = information->Processor.GroupCount;
            if (groupCount == 0 ||
                groupCount > (recordSize - coreGroupMaskOffset) / sizeof(GROUP_AFFINITY))
            {
                error = L"물리 코어 구성 데이터가 올바르지 않습니다.";
                return std::nullopt;
            }

            const std::uint32_t coreIndex = physicalCoreIndex++;
            if (const GROUP_AFFINITY* groupMask = FindCoreGroupMask(*information))
            {
                const KAFFINITY availableToGame = groupMask->Mask & gameMask;
                const KAFFINITY availableToSystem = groupMask->Mask & systemMask;
                if (availableToGame == 0 || availableToSystem == 0)
                {
                    offset += recordSize;
                    continue;
                }

                double peakUsage = 0.0;
                bool hasPeakUsage = false;
                int highestEfficiencyClass = -1;
                for (int bit = 0; bit < static_cast<int>(sizeof(KAFFINITY) * 8); ++bit)
                {
                    const KAFFINITY candidate = static_cast<KAFFINITY>(1) << bit;
                    if ((availableToSystem & candidate) != 0)
                    {
                        if (static_cast<std::size_t>(bit) >= processorUsage.size() ||
                            processorUsage[bit] < 0.0)
                        {
                            error = L"일부 논리 프로세서의 사용률 표본이 비어 있습니다.";
                            return std::nullopt;
                        }
                        if (!hasPeakUsage || processorUsage[bit] > peakUsage)
                        {
                            peakUsage = processorUsage[bit];
                            hasPeakUsage = true;
                        }
                        if (static_cast<std::size_t>(bit) < efficiencyClasses.size())
                            highestEfficiencyClass = std::max(highestEfficiencyClass,
                                efficiencyClasses[bit]);
                    }
                }

                KAFFINITY selectedProcessor = 0;
                double selectedUsage = 101.0;
                for (int bit = 0; bit < static_cast<int>(sizeof(KAFFINITY) * 8); ++bit)
                {
                    const KAFFINITY candidate = static_cast<KAFFINITY>(1) << bit;
                    if ((availableToGame & candidate) != 0)
                    {
                        if (static_cast<std::size_t>(bit) >= processorUsage.size() ||
                            processorUsage[bit] < 0.0)
                        {
                            error = L"게임 논리 프로세서의 사용률 표본이 비어 있습니다.";
                            return std::nullopt;
                        }
                        if (processorUsage[bit] >= selectedUsage)
                            continue;
                        selectedProcessor = candidate;
                        selectedUsage = processorUsage[bit];
                    }
                }

                if (selectedProcessor != 0)
                {
                    CpuCoreCandidate core{};
                    core.PhysicalCoreIndex = coreIndex;
                    core.LogicalProcessorIndex = static_cast<std::uint32_t>(std::countr_zero(selectedProcessor));
                    core.LogicalProcessor.Group = 0;
                    core.LogicalProcessor.Mask = selectedProcessor;
                    core.PeakUsagePercent = peakUsage;
                    core.SelectedUsagePercent = selectedUsage;
                    if (highestEfficiencyClass >= 0)
                        core.EfficiencyClass = static_cast<std::uint8_t>(highestEfficiencyClass);
                    candidates.push_back(core);
                }
            }

            offset += recordSize;
        }

        if (candidates.size() < 2)
        {
            error = L"게임에서 사용할 수 있는 물리 코어 후보가 두 곳 미만입니다.";
            return std::nullopt;
        }

        std::sort(candidates.begin(), candidates.end(),
            [](const CpuCoreCandidate& left, const CpuCoreCandidate& right)
            {
                if (left.PeakUsagePercent != right.PeakUsagePercent)
                    return left.PeakUsagePercent < right.PeakUsagePercent;
                return left.PhysicalCoreIndex < right.PhysicalCoreIndex;
            });

        const std::size_t candidateCount = (candidates.size() + 1) / 2;
        if (candidateCount < 2)
        {
            error = L"사용률 상위 절반을 제외하면 게임 코어 두 곳을 남길 수 없습니다.";
            return std::nullopt;
        }
        for (std::size_t index = 0; index < candidateCount; ++index)
            candidates[index].Candidate = true;

        const bool allCandidateClassesAvailable = std::all_of(candidates.begin(), candidates.end(),
            [](const CpuCoreCandidate& core)
            {
                return !core.Candidate || core.EfficiencyClass.has_value();
            });
        std::sort(candidates.begin(), candidates.end(),
            [allCandidateClassesAvailable](const CpuCoreCandidate& left,
                const CpuCoreCandidate& right)
            {
                if (left.Candidate != right.Candidate)
                    return left.Candidate;
                if (allCandidateClassesAvailable &&
                    left.EfficiencyClass != right.EfficiencyClass)
                    return left.EfficiencyClass > right.EfficiencyClass;
                if (left.PeakUsagePercent != right.PeakUsagePercent)
                    return left.PeakUsagePercent < right.PeakUsagePercent;
                return left.PhysicalCoreIndex < right.PhysicalCoreIndex;
            });

        error.clear();
        return candidates;
    }

    std::optional<CpuPlan> SelectCpuPlan(HANDLE gameProcess, std::wstring& error) // 서로 다른 저사용 물리 코어 2곳 자동 선택
    {
        const auto candidates = FindCpuCoreCandidates(gameProcess, error);
        if (!candidates)
            return std::nullopt;

        KAFFINITY gameMask = 0;
        KAFFINITY systemMask = 0;
        if (!GetProcessAffinityMask(gameProcess, &gameMask, &systemMask))
        {
            error = ErrorMessage(L"CPU 목록을 읽지 못했습니다");
            return std::nullopt;
        }

        GROUP_AFFINITY dominant{};
        dominant.Group = 0;
        const CpuCoreCandidate& dominantCore = (*candidates)[0];
        const CpuCoreCandidate& workerCore = (*candidates)[1];

        dominant.Mask = dominantCore.LogicalProcessor.Mask;

        GROUP_AFFINITY workers{};
        workers.Group = 0;
        workers.Mask = workerCore.LogicalProcessor.Mask;

        CpuPlan plan{};
        plan.Game.Group = 0;
        plan.Game.Mask = dominant.Mask | workers.Mask;
        plan.Dominant = dominant;
        plan.Workers = workers;
        plan.DominantCore = dominantCore;
        plan.WorkerCore = workerCore;

        if (std::popcount(systemMask & ~plan.Game.Mask) == 0)
        {
            error = L"게임 전용 CPU 두 곳 외에 사용할 논리 프로세서가 없습니다.";
            return std::nullopt;
        }

        error.clear();
        return plan;
    }

    bool ProcessAffinityController::HasPendingRecovery() const noexcept // 프로세스·스레드 affinity 원복 여부 확인
    {
        return std::any_of(pending_.begin(), pending_.end(),
            [](const PendingRestore& item)
            {
                return item.NeedsRestore || !item.Threads.empty();
            });
    }

    bool ProcessAffinityController::CaptureThreadAffinities(DWORD processId,
        KAFFINITY processMask, std::vector<PendingThreadRestore>& threads,
        DWORD& errorCode, std::wstring& error) // 프로세스 범위 변경 전 스레드 값 저장
    {
        errorCode = ERROR_SUCCESS;
        UniqueHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0));
        if (!snapshot.IsValid())
        {
            errorCode = GetLastError();
            error = ErrorMessage(L"프로세스 스레드 목록을 읽지 못했습니다", errorCode);
            return false;
        }

        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        BOOL hasEntry = Thread32First(snapshot.Get(), &entry);
        if (!hasEntry && GetLastError() != ERROR_NO_MORE_FILES)
        {
            errorCode = GetLastError();
            error = ErrorMessage(L"프로세스 스레드 목록을 읽지 못했습니다", errorCode);
            return false;
        }

        while (hasEntry)
        {
            if (entry.th32OwnerProcessID == processId)
            {
                UniqueHandle thread(OpenThread(
                    THREAD_QUERY_INFORMATION | THREAD_SET_INFORMATION | SYNCHRONIZE,
                    FALSE, entry.th32ThreadID));
                if (!thread.IsValid())
                {
                    const DWORD openError = GetLastError();
                    if (openError != ERROR_INVALID_PARAMETER)
                    {
                        errorCode = openError;
                        error = ErrorMessage(L"프로세스 스레드의 CPU 범위를 저장할 수 없습니다",
                            errorCode);
                        return false;
                    }
                }
                else if (GetProcessIdOfThread(thread.Get()) == processId)
                {
                    FILETIME creation{}, exit{}, kernel{}, user{};
                    GROUP_AFFINITY affinity{};
                    bool threadRead = GetThreadTimes(thread.Get(), &creation, &exit, &kernel, &user) != FALSE;
                    DWORD threadError = threadRead ? ERROR_SUCCESS : GetLastError();
                    if (threadRead)
                    {
                        threadRead = GetThreadGroupAffinity(thread.Get(), &affinity) != FALSE;
                        if (!threadRead) threadError = GetLastError();
                    }

                    if (!threadRead)
                    {
                        if (WaitForSingleObject(thread.Get(), 0) != WAIT_OBJECT_0)
                        {
                            errorCode = threadError;
                            error = ErrorMessage(L"프로세스 스레드의 CPU 범위를 저장할 수 없습니다",
                                errorCode);
                            return false;
                        }
                    }
                    else
                    {
                        if (affinity.Group != 0 || affinity.Mask != processMask)
                        {
                            threads.emplace_back(std::move(thread), entry.th32ThreadID,
                                FileTimeValue(creation), affinity);
                        }
                    }
                }
            }

            entry.dwSize = sizeof(entry);
            hasEntry = Thread32Next(snapshot.Get(), &entry);
            if (!hasEntry && GetLastError() != ERROR_NO_MORE_FILES)
            {
                errorCode = GetLastError();
                error = ErrorMessage(L"프로세스 스레드 목록을 끝까지 읽지 못했습니다",
                    errorCode);
                return false;
            }
        }

        return true;
    }

    bool ProcessAffinityController::Apply(HANDLE gameProcess, DWORD gameProcessId,
        std::uint64_t gameCreationTime, const std::wstring& gamePath,
        const CpuPlan& plan, const std::wstring& journalPath,
        const std::atomic<bool>& stopRequested,
        std::wstring& message) // 게임에 CPU 2곳 배정·다른 프로세스에서 제외
    {
        std::erase_if(pending_, [](const PendingRestore& item)
        {
            return WaitForSingleObject(item.Process.Get(), 0) == WAIT_OBJECT_0;
        });

        if (!ValidateTarget(gameProcess, gameProcessId, gameCreationTime, gamePath, message))
            return false;

        UniqueHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
        if (!snapshot.IsValid())
        {
            message = ErrorMessage(L"프로세스 목록을 읽지 못했습니다");
            return false;
        }

        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        BOOL hasEntry = Process32FirstW(snapshot.Get(), &entry);
        if (!hasEntry && GetLastError() != ERROR_NO_MORE_FILES)
        {
            message = ErrorMessage(L"프로세스 목록을 읽지 못했습니다");
            return false;
        }

        bool foundGame = false;
        std::size_t restrictedProcessCount = 0;
        std::vector<std::wstring> restrictedProcesses;
        const auto recordRestricted = [&restrictedProcessCount, &restrictedProcesses](
            const std::wstring& reason, const PROCESSENTRY32W& processEntry)
        {
            ++restrictedProcessCount;
            if (restrictedProcesses.size() < 3)
            {
                restrictedProcesses.push_back(reason + L": " + processEntry.szExeFile +
                    L" (PID " + std::to_wstring(processEntry.th32ProcessID) + L")");
            }
        };
        const auto describeProcess = [](const std::wstring& message,
            const PROCESSENTRY32W& processEntry)
        {
            return message + L" " + processEntry.szExeFile + L" (PID " +
                std::to_wstring(processEntry.th32ProcessID) + L").";
        };
        while (hasEntry)
        {
            if (stopRequested.load()) return true;

            const DWORD processId = entry.th32ProcessID;
            if (processId > 4)
            {
                bool skipCurrentProcess = false;
                UniqueHandle process = OpenProcessForAffinity(processId,
                    gameProcess, gameProcessId);
                if (!process.IsValid())
                {
                    const DWORD openError = GetLastError();
                    if (openError == ERROR_ACCESS_DENIED && processId != gameProcessId)
                    {
                        recordRestricted(L"접근 거부(프로세스 열기)", entry);
                    }
                    else if (openError != ERROR_INVALID_PARAMETER)
                    {
                        message = describeProcess(ErrorMessage(
                            L"프로세스 CPU 범위를 변경할 수 없습니다", openError), entry);
                        return false;
                    }
                }
                else if (IsCriticalProcess(process.Get()))
                {
                    recordRestricted(L"Windows 중요 프로세스", entry);
                }
                else
                {
                    std::uint64_t creationTime = 0;
                    if (!ReadProcessIdentity(process.Get(), processId, creationTime))
                    {
                        const DWORD identityError = GetLastError();
                        const DWORD wait = WaitForSingleObject(process.Get(), 0);
                        if (wait != WAIT_OBJECT_0)
                        {
                            if (identityError == ERROR_ACCESS_DENIED && processId != gameProcessId)
                                recordRestricted(L"접근 거부(프로세스 확인)", entry);
                            else
                            {
                                message = describeProcess(ErrorMessage(
                                    L"프로세스 신원을 확인할 수 없습니다", identityError), entry);
                                return false;
                            }
                        }
                    }
                    else
                    {
                        const bool isGame = processId == gameProcessId &&
                            creationTime == gameCreationTime;
                        if (processId == gameProcessId && !isGame)
                        {
                            message = L"게임 PID의 생성 시각이 달라졌습니다.";
                            return false;
                        }
                        if (isGame) foundGame = true;

                        KAFFINITY currentMask = 0;
                        KAFFINITY systemMask = 0;
                        if (!GetProcessAffinityMask(process.Get(), &currentMask, &systemMask))
                        {
                            const DWORD queryError = GetLastError();
                            if (queryError == ERROR_ACCESS_DENIED && !isGame)
                                recordRestricted(L"접근 거부(CPU 범위 조회)", entry);
                            else
                            {
                                message = describeProcess(ErrorMessage(
                                    L"프로세스 CPU 범위를 읽지 못했습니다", queryError), entry);
                                return false;
                            }
                        }
                        else
                        {
                            const KAFFINITY desiredMask = isGame
                                ? plan.Game.Mask
                                : ((currentMask & ~plan.Game.Mask) != 0
                                    ? (currentMask & ~plan.Game.Mask)
                                    : (systemMask & ~plan.Game.Mask));
                            if (desiredMask == 0 || (desiredMask & ~systemMask) != 0)
                            {
                                message = describeProcess(
                                    L"프로세스에 사용할 CPU 범위를 만들지 못했습니다.", entry);
                                return false;
                            }

                            auto pending = std::find_if(pending_.begin(), pending_.end(),
                                [processId, creationTime](const PendingRestore& item)
                                {
                                    return item.ProcessId == processId &&
                                        item.CreationFileTime == creationTime;
                                });

                            KAFFINITY originalMask = currentMask;
                            if (pending != pending_.end())
                            {
                                originalMask = pending->OriginalMask;
                                if (currentMask != pending->AppliedMask && currentMask != originalMask)
                                {
                                    message = describeProcess(
                                        L"프로세스 CPU 범위가 외부에서 변경되었습니다.", entry);
                                    return false;
                                }
                            }

                            if (currentMask != desiredMask)
                            {
                                const bool hadPending = pending != pending_.end();
                                const KAFFINITY previousAppliedMask = hadPending
                                    ? pending->AppliedMask : 0;
                                const bool previousNeedsRestore = hadPending && pending->NeedsRestore;
                                bool createdPending = false;

                                if (!hadPending)
                                {
                                    std::vector<PendingThreadRestore> originalThreads;
                                    DWORD captureError = ERROR_SUCCESS;
                                    if (!CaptureThreadAffinities(processId, currentMask,
                                        originalThreads, captureError, message))
                                    {
                                        if (captureError == ERROR_ACCESS_DENIED && !isGame)
                                        {
                                            recordRestricted(L"접근 거부(스레드 범위 저장)", entry);
                                            skipCurrentProcess = true;
                                        }
                                        else
                                        {
                                            message = describeProcess(message, entry);
                                            return false;
                                        }
                                    }
                                    else
                                    {
                                        for (const auto& thread : originalThreads)
                                        {
                                            if (!RecordImplicitThreadAffinity(journalPath,
                                                processId, creationTime, thread.ThreadId,
                                                thread.CreationFileTime, thread.OriginalAffinity,
                                                currentMask, desiredMask, message))
                                                return false;
                                        }
                                        if (!RecordProcessAffinity(journalPath, processId,
                                            creationTime, currentMask, desiredMask, message))
                                            return false;
                                        pending_.emplace_back(std::move(process), processId,
                                            creationTime, currentMask, desiredMask,
                                            std::move(originalThreads));
                                        pending = std::prev(pending_.end());
                                        createdPending = true;
                                    }
                                }
                                else
                                {
                                    if (!RecordProcessAffinity(journalPath, processId,
                                        creationTime, currentMask, desiredMask, message))
                                        return false;
                                    pending->AppliedMask = desiredMask;
                                    pending->NeedsRestore = pending->OriginalMask != desiredMask;
                                }

                                if (!skipCurrentProcess &&
                                    !SetProcessAffinityMask(pending->Process.Get(), desiredMask))
                                {
                                    const DWORD applyError = GetLastError();
                                    if (applyError == ERROR_ACCESS_DENIED && !isGame)
                                    {
                                        if (createdPending)
                                            pending_.erase(pending);
                                        else
                                        {
                                            pending->AppliedMask = previousAppliedMask;
                                            pending->NeedsRestore = previousNeedsRestore;
                                        }
                                        recordRestricted(L"접근 거부(CPU 범위 적용)", entry);
                                        skipCurrentProcess = true;
                                    }
                                    else
                                    {
                                        message = describeProcess(ErrorMessage(
                                            L"프로세스 CPU 범위를 적용하지 못했습니다", applyError), entry);
                                        return false;
                                    }
                                }

                                if (!skipCurrentProcess)
                                {
                                    KAFFINITY verifiedMask = 0;
                                    KAFFINITY verifiedSystemMask = 0;
                                    if (!GetProcessAffinityMask(pending->Process.Get(),
                                        &verifiedMask, &verifiedSystemMask) || verifiedMask != desiredMask)
                                    {
                                        message = describeProcess(
                                            L"프로세스 CPU 범위 적용 후 읽기 검증에 실패했습니다.", entry);
                                        return false;
                                    }
                                }
                            }
                            else if (pending != pending_.end())
                            {
                                pending->AppliedMask = desiredMask;
                                pending->NeedsRestore = pending->OriginalMask != desiredMask;
                            }
                        }
                    }
                }
            }

            entry.dwSize = sizeof(entry);
            hasEntry = Process32NextW(snapshot.Get(), &entry);
            if (!hasEntry && GetLastError() != ERROR_NO_MORE_FILES)
            {
                message = ErrorMessage(L"프로세스 목록을 끝까지 읽지 못했습니다");
                return false;
            }
        }

        if (!foundGame)
        {
            message = L"게임 프로세스를 전체 프로세스 목록에서 찾지 못했습니다.";
            return false;
        }

        lastError_.clear();
        message = L"게임과 접근 가능한 프로세스의 CPU 범위를 적용하고 확인했습니다.";
        if (restrictedProcessCount > 0)
        {
            message += L" 제한으로 건너뛴 프로세스 " +
                std::to_wstring(restrictedProcessCount) + L"개: ";
            for (std::size_t index = 0; index < restrictedProcesses.size(); ++index)
            {
                if (index > 0) message += L", ";
                message += restrictedProcesses[index];
            }
            if (restrictedProcessCount > restrictedProcesses.size())
            {
                message += L" 외 " +
                    std::to_wstring(restrictedProcessCount - restrictedProcesses.size()) + L"개";
            }
            message += L". 해당 프로세스는 게임 CPU에서 실행될 수 있습니다.";
        }
        return true;
    }

    bool ProcessAffinityController::TryRestore() // 저장된 프로세스·스레드 affinity 복구·검증
    {
        lastError_.clear();
        auto item = pending_.begin();
        while (item != pending_.end())
        {
            const DWORD wait = WaitForSingleObject(item->Process.Get(), 0);
            if (wait == WAIT_OBJECT_0)
            {
                item = pending_.erase(item);
                continue;
            }
            if (wait != WAIT_TIMEOUT || !SameProcessIdentity(item->Process.Get(),
                item->ProcessId, item->CreationFileTime))
            {
                lastError_ = L"복구 대상 프로세스의 신원을 확인할 수 없습니다.";
                ++item;
                continue;
            }

            KAFFINITY currentMask = 0;
            KAFFINITY systemMask = 0;
            if (!GetProcessAffinityMask(item->Process.Get(), &currentMask, &systemMask))
            {
                lastError_ = ErrorMessage(L"복구 전 프로세스 CPU 범위를 읽지 못했습니다");
                ++item;
                continue;
            }

            if (currentMask == item->OriginalMask)
            {
                item->NeedsRestore = false;
            }
            else if (currentMask != item->AppliedMask)
            {
                lastError_ = L"프로세스 CPU 범위가 외부에서 변경되어 안전한 원복을 확인할 수 없습니다.";
                ++item;
                continue;
            }
            else if (item->NeedsRestore)
            {
                KAFFINITY verifiedMask = 0;
                KAFFINITY verifiedSystemMask = 0;
                if (!SetProcessAffinityMask(item->Process.Get(), item->OriginalMask) ||
                    !GetProcessAffinityMask(item->Process.Get(), &verifiedMask, &verifiedSystemMask) ||
                    verifiedMask != item->OriginalMask)
                {
                    lastError_ = L"원래 프로세스 CPU 범위를 복구하거나 검증하지 못했습니다.";
                    ++item;
                    continue;
                }
                item->NeedsRestore = false;
            }

            GROUP_AFFINITY processOriginal{};
            processOriginal.Group = 0;
            processOriginal.Mask = item->OriginalMask;
            GROUP_AFFINITY processApplied{};
            processApplied.Group = 0;
            processApplied.Mask = item->AppliedMask;

            auto thread = item->Threads.begin();
            while (thread != item->Threads.end())
            {
                const DWORD threadWait = WaitForSingleObject(thread->Thread.Get(), 0);
                if (threadWait == WAIT_OBJECT_0)
                {
                    thread = item->Threads.erase(thread);
                    continue;
                }
                if (threadWait != WAIT_TIMEOUT || !MatchesThreadIdentity(
                    thread->Thread.Get(), item->ProcessId, thread->ThreadId,
                    thread->CreationFileTime))
                {
                    lastError_ = L"복구 대상 프로세스 스레드의 신원을 확인할 수 없습니다.";
                    ++thread;
                    continue;
                }

                GROUP_AFFINITY current{};
                if (!GetThreadGroupAffinity(thread->Thread.Get(), &current))
                {
                    lastError_ = L"복구 전 프로세스 스레드 CPU 범위를 읽지 못했습니다.";
                    ++thread;
                    continue;
                }
                if (SameAffinity(current, thread->OriginalAffinity))
                {
                    thread = item->Threads.erase(thread);
                    continue;
                }
                if (!SameAffinity(current, processOriginal) &&
                    !SameAffinity(current, processApplied))
                {
                    lastError_ = L"프로세스 스레드 CPU 범위가 외부에서 변경되어 안전한 원복을 확인할 수 없습니다.";
                    ++thread;
                    continue;
                }
                if (!SetThreadGroupAffinity(thread->Thread.Get(),
                    &thread->OriginalAffinity, nullptr) ||
                    !GetThreadGroupAffinity(thread->Thread.Get(), &current) ||
                    !SameAffinity(current, thread->OriginalAffinity))
                {
                    lastError_ = L"원래 프로세스 스레드 CPU 범위를 복구하거나 검증하지 못했습니다.";
                    ++thread;
                    continue;
                }
                thread = item->Threads.erase(thread);
            }

            if (!item->NeedsRestore && item->Threads.empty())
                item = pending_.erase(item);
            else
                ++item;
        }

        return !HasPendingRecovery();
    }
}
