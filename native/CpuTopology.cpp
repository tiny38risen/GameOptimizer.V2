#include "CpuTopology.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <span>
#include <vector>

namespace gameoptimizer::engine
{
    namespace
    {
        using Info = SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX;

        struct CacheGroup
        {
            KAFFINITY Mask = 0;
            std::uint64_t SizeBytes = 0;
        };

        [[nodiscard]] std::vector<std::optional<std::uint8_t>> ReadEfficiencyClasses(
            DWORD processorCount)
        {
            std::vector<std::optional<std::uint8_t>> classes(processorCount);
            ULONG size = 0;
            if (GetSystemCpuSetInformation(nullptr, 0, &size, nullptr, 0) ||
                GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0)
                return classes;

            std::vector<std::byte> buffer(size);
            if (!GetSystemCpuSetInformation(
                reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buffer.data()),
                size, &size, nullptr, 0))
                return classes;

            const auto parsed = ForEachRecord<SYSTEM_CPU_SET_INFORMATION>(
                std::span<const std::byte>(buffer).first(size),
                sizeof(SYSTEM_CPU_SET_INFORMATION),
                [&classes, processorCount](const SYSTEM_CPU_SET_INFORMATION& info)
                {
                    if (info.Type == CpuSetInformation && info.CpuSet.Group == 0 &&
                        info.CpuSet.LogicalProcessorIndex < processorCount)
                        classes[info.CpuSet.LogicalProcessorIndex] =
                            info.CpuSet.EfficiencyClass;
                });
            if (!parsed) std::ranges::fill(classes, std::nullopt);
            return classes;
        }

        [[nodiscard]] KAFFINITY Group0Mask(
            const GROUP_AFFINITY* masks, std::size_t count) noexcept
        {
            KAFFINITY mask = 0;
            for (const GROUP_AFFINITY& group : std::span(masks, count))
                if (group.Group == 0) mask |= group.Mask;
            return mask;
        }
    }

    Result<std::vector<PhysicalCore>> ReadCpuTopology()
    {
        if (GetActiveProcessorGroupCount() != 1)
            return FailText(L"이 PC의 CPU 그룹 구성을 자동 분석할 수 없습니다.");

        DWORD size = 0;
        if (GetLogicalProcessorInformationEx(RelationAll, nullptr, &size) ||
            GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0)
            return Fail(L"CPU 구성을 읽지 못했습니다");

        std::vector<std::byte> buffer(size);
        if (!GetLogicalProcessorInformationEx(RelationAll,
            reinterpret_cast<Info*>(buffer.data()), &size))
            return Fail(L"CPU 구성을 읽지 못했습니다");

        std::vector<PhysicalCore> cores;
        std::vector<CacheGroup> caches;
        std::uint32_t coreIndex = 0;

        const auto parsed = ForEachRecord<Info>(
            std::span<const std::byte>(buffer).first(size), offsetof(Info, Processor),
            [&](const Info& info, std::size_t recordSize) -> bool
            {
                if (info.Relationship == RelationProcessorCore)
                {
                    constexpr std::size_t masksOffset =
                        offsetof(Info, Processor.GroupMask);
                    if (recordSize < masksOffset) return false;
                    const std::size_t count = info.Processor.GroupCount;
                    if (count == 0 || count >
                        (recordSize - masksOffset) / sizeof(GROUP_AFFINITY))
                        return false;

                    const std::uint32_t index = coreIndex++;
                    const KAFFINITY mask = Group0Mask(
                        info.Processor.GroupMask, count);
                    if (mask != 0)
                        cores.push_back({ .Index = index, .LogicalMask = mask });
                }
                else if (info.Relationship == RelationCache)
                {
                    constexpr std::size_t masksOffset =
                        offsetof(Info, Cache.GroupMask);
                    if (recordSize < masksOffset + sizeof(GROUP_AFFINITY)) return false;
                    if (info.Cache.Level != 3) return true;

                    const std::size_t count = info.Cache.GroupCount == 0
                        ? 1 : info.Cache.GroupCount;
                    if (count > (recordSize - masksOffset) / sizeof(GROUP_AFFINITY))
                        return false;

                    const KAFFINITY mask = Group0Mask(info.Cache.GroupMasks, count);
                    if (mask != 0)
                        caches.push_back({ .Mask = mask,
                            .SizeBytes = info.Cache.CacheSize });
                }
                return true;
            });
        if (!parsed) return std::unexpected(parsed.error());
        if (cores.empty()) return FailText(L"물리 코어 정보를 찾지 못했습니다.");

        const DWORD processorCount = GetActiveProcessorCount(0);
        if (processorCount == 0)
            return Fail(L"논리 프로세서 수를 읽지 못했습니다");
        const auto efficiency = ReadEfficiencyClasses(processorCount);

        for (PhysicalCore& core : cores)
        {
            if (caches.empty())
            {
                core.L3Index = 0;
            }
            else if (const auto cache = std::ranges::find_if(caches,
                [&](const CacheGroup& candidate)
                {
                    return (core.LogicalMask & ~candidate.Mask) == 0;
                }); cache != caches.end())
            {
                core.L3Index = static_cast<std::uint32_t>(cache - caches.begin());
                core.L3SizeBytes = cache->SizeBytes;
            }
            else
            {
                core.L3Index = static_cast<std::uint32_t>(caches.size() + core.Index);
            }

            for (KAFFINITY bits = core.LogicalMask; bits != 0; bits &= bits - 1)
            {
                const auto lp = static_cast<std::size_t>(std::countr_zero(bits));
                if (lp < efficiency.size() && efficiency[lp])
                    core.EfficiencyClass = std::max(
                        core.EfficiencyClass.value_or(0), *efficiency[lp]);
            }
        }
        return cores;
    }
}
