#pragma once

#include <filesystem>
#include <iostream>
#include <optional>
#include <string_view>

namespace gameoptimizer::engine::tests
{
    class TestContext
    {
    public:
        void Check(bool passed, std::wstring_view name)
        {
            ++checks_;
            if (passed) return;
            ++failures_;
            std::wcerr << L"FAIL: " << name << L'\n';
        }

        [[nodiscard]] int Checks() const noexcept { return checks_; }
        [[nodiscard]] int Failures() const noexcept { return failures_; }

    private:
        int checks_ = 0;
        int failures_ = 0;
    };

    void RunEngineResultTests(TestContext& context);
    void RunCoreSelectionTests(TestContext& context);
    void RunCoreSelectionFixtures(TestContext& context, const std::filesystem::path& directory);
}
