#pragma once

#include "EngineInternals.h"

#include <format>
#include <mutex>
#include <string>
#include <utility>

namespace gameoptimizer::engine
{
    class DiagnosticLog
    {
    public:
        explicit DiagnosticLog(std::wstring path);
        ~DiagnosticLog();
        DiagnosticLog(const DiagnosticLog&) = delete;
        DiagnosticLog& operator=(const DiagnosticLog&) = delete;

        template <class... Args>
        void Write(std::wformat_string<Args...> format, Args&&... args)
        {
            try
            {
                WriteLine(std::format(format, std::forward<Args>(args)...));
            }
            catch (...)
            {
                // Diagnostics must never stop the optimizer.
            }
        }

    private:
        void WriteLine(const std::wstring& line) noexcept;

        UniqueHandle file_;
        std::mutex mutex_;
    };
}
