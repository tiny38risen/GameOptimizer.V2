#pragma once

#include "EngineInternals.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace gameoptimizer::engine
{
    enum class ErrorKind : std::uint8_t
    {
        Cancelled,
        TargetExited,
        InvalidData,
        Win32,
        Pdh,
    };

    struct EngineError
    {
        ErrorKind Kind = ErrorKind::InvalidData;
        std::wstring Message;
        std::uint32_t Code = 0;

        [[nodiscard]] bool Is(ErrorKind kind) const noexcept { return Kind == kind; }
    };

    template <class T>
    using Result = std::expected<T, EngineError>;

    [[nodiscard]] inline std::unexpected<EngineError> Fail(
        const wchar_t* message, DWORD code = GetLastError())
    {
        return std::unexpected(EngineError{ .Kind = ErrorKind::Win32,
            .Message = ErrorMessage(message, code), .Code = code });
    }

    [[nodiscard]] inline std::unexpected<EngineError> FailPdh(
        std::wstring_view message, LONG status)
    {
        const auto code = static_cast<std::uint32_t>(status);
        return std::unexpected(EngineError{ .Kind = ErrorKind::Pdh,
            .Message = std::format(L"{} (PDH 0x{:08X})", message, code), .Code = code });
    }

    [[nodiscard]] inline std::unexpected<EngineError> FailText(std::wstring message)
    {
        return std::unexpected(EngineError{ .Kind = ErrorKind::InvalidData,
            .Message = std::move(message) });
    }

    [[nodiscard]] inline std::unexpected<EngineError> TargetExited(std::wstring message)
    {
        return std::unexpected(EngineError{ .Kind = ErrorKind::TargetExited,
            .Message = std::move(message) });
    }

    [[nodiscard]] inline std::unexpected<EngineError> Cancelled()
    {
        return std::unexpected(EngineError{ .Kind = ErrorKind::Cancelled,
            .Message = L"중단 요청을 받았습니다." });
    }

    template <class T>
    [[nodiscard]] bool IsCancelled(const Result<T>& result) noexcept
    {
        return !result && result.error().Is(ErrorKind::Cancelled);
    }

    [[nodiscard]] constexpr std::wstring_view ToString(ErrorKind kind) noexcept
    {
        switch (kind)
        {
        case ErrorKind::Cancelled: return L"cancelled";
        case ErrorKind::TargetExited: return L"target-exited";
        case ErrorKind::InvalidData: return L"invalid-data";
        case ErrorKind::Win32: return L"win32";
        case ErrorKind::Pdh: return L"pdh";
        }
        return L"unknown";
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
