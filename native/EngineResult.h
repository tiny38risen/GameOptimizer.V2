#pragma once

#include "EngineInternals.h"

#include <algorithm>
#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <type_traits>
#include <utility>

namespace gameoptimizer::engine
{
    struct EngineError
    {
        std::wstring Message;
        DWORD Code = ERROR_SUCCESS;
    };

    template <class T>
    using Result = std::expected<T, EngineError>;

    [[nodiscard]] inline std::unexpected<EngineError> Fail(
        const wchar_t* message, DWORD code = GetLastError())
    {
        return std::unexpected(EngineError{ ErrorMessage(message, code), code });
    }

    [[nodiscard]] inline std::unexpected<EngineError> FailText(std::wstring message)
    {
        return std::unexpected(EngineError{ std::move(message), ERROR_INVALID_DATA });
    }

    [[nodiscard]] inline std::unexpected<EngineError> Cancelled()
    {
        return std::unexpected(EngineError{ L"중단 요청을 받았습니다.", ERROR_CANCELLED });
    }

    // Visits variable-length records with a Size field. A bool visitor can
    // reject malformed content; visitors may optionally also receive record size.
    template <class Record, class Visitor>
    [[nodiscard]] Result<void> ForEachRecord(std::span<const std::byte> buffer,
        std::size_t minimumSize, Visitor&& visit)
    {
        constexpr std::size_t sizeFieldEnd = offsetof(Record, Size) + sizeof(Record::Size);
        const std::size_t headerSize = std::max(minimumSize, sizeFieldEnd);

        for (std::size_t offset = 0; offset < buffer.size();)
        {
            const std::size_t remaining = buffer.size() - offset;
            if (remaining < sizeFieldEnd)
                return FailText(L"시스템 정보 레코드가 잘렸습니다.");

            const auto& record = *reinterpret_cast<const Record*>(buffer.data() + offset);
            const std::size_t recordSize = record.Size;
            if (recordSize < headerSize || recordSize > remaining)
                return FailText(L"시스템 정보 레코드 크기가 올바르지 않습니다.");

            const auto call = [&]
            {
                if constexpr (std::is_invocable_v<Visitor&, const Record&, std::size_t>)
                    return visit(record, recordSize);
                else
                    return visit(record);
            };
            if constexpr (std::is_same_v<decltype(call()), bool>)
            {
                if (!call())
                    return FailText(L"시스템 정보 레코드 내용이 올바르지 않습니다.");
            }
            else
            {
                call();
            }

            offset += recordSize;
        }
        return {};
    }
}
