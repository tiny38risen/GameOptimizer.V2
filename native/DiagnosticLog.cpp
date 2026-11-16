#include "DiagnosticLog.h"

#include <Windows.h>

#include <format>
#include <vector>

namespace gameoptimizer::engine
{
    DiagnosticLog::DiagnosticLog(std::wstring path)
    {
        if (!path.empty())
            file_.Reset(CreateFileW(path.c_str(), FILE_APPEND_DATA,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                FILE_ATTRIBUTE_NORMAL, nullptr));
    }

    DiagnosticLog::~DiagnosticLog() = default;

    void DiagnosticLog::WriteLine(const std::wstring& line) noexcept
    {
        if (!file_.IsValid()) return;

        try
        {
            SYSTEMTIME now{};
            GetLocalTime(&now);
            const std::wstring entry = std::format(
                L"{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03} {}\r\n",
                now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
                now.wSecond, now.wMilliseconds, line);
            const int byteCount = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                entry.data(), static_cast<int>(entry.size()), nullptr, 0, nullptr, nullptr);
            if (byteCount <= 0) return;

            std::vector<char> utf8(static_cast<std::size_t>(byteCount));
            if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, entry.data(),
                static_cast<int>(entry.size()), utf8.data(), byteCount, nullptr, nullptr) != byteCount)
                return;

            std::lock_guard lock(mutex_);
            DWORD written = 0;
            WriteFile(file_.Get(), utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
        }
        catch (...)
        {
            // Diagnostics must never stop the optimizer.
        }
    }
}
