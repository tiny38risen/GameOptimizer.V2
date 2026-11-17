#include "TestSupport.h"

#include "../CoreSelection.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <expected>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace gameoptimizer::engine::tests
{
    namespace
    {
        // ---- Hand-written rule cases ------------------------------------------------

        [[nodiscard]] CoreMeasurement MakeCore(std::uint32_t index, std::uint32_t firstLp,
            std::uint32_t l3, double speed, std::initializer_list<double> interruptDpc)
        {
            CoreMeasurement core{ .Core = { .Index = index, .L3Index = l3, .EfficiencyClass = std::uint8_t{ 0 } },
                .RelativeSpeedPercent = speed };
            std::uint32_t lp = firstLp;
            for (const double value : interruptDpc)
            {
                core.Core.LogicalMask |= KAFFINITY{ 1 } << lp;
                core.Lps.push_back({ .Index = lp++, .InterruptDpcPercent = value });
            }
            return core;
        }

        [[nodiscard]] std::uint32_t CoreIndex(std::span<const CoreMeasurement> cores,
            const LpChoice& choice)
        {
            return cores[choice.Core].Core.Index;
        }

        [[nodiscard]] std::uint32_t LpIndex(std::span<const CoreMeasurement> cores,
            const LpChoice& choice)
        {
            return cores[choice.Core].Lps[choice.Lp].Index;
        }

        constexpr SelectionConfig Config{ .SpeedTolerancePercent = 1.0,
            .InterruptDpcTolerancePercent = 0.5 };
        constexpr KAFFINITY AllLps = ~KAFFINITY{ 0 };

        void SkipsL3GroupsWithOneCore(TestContext& context)
        {
            // Core 0 is fastest but alone in its L3, so no same-L3 worker could exist.
            const std::vector cores{
                MakeCore(0, 0, 0, 100.0, { 0.0, 0.0 }),
                MakeCore(1, 2, 1, 95.0, { 0.0, 0.0 }),
                MakeCore(2, 4, 1, 94.0, { 0.0, 0.0 }) };
            const auto selection = SelectCores(cores, AllLps, 0x3F, Config);
            context.Check(selection && CoreIndex(cores, selection->Dominant) == 1 &&
                CoreIndex(cores, selection->Worker) == 2,
                L"L3 groups with fewer than two cores are excluded");
        }

        void WorkerSharesDominantL3(TestContext& context)
        {
            // Cores 2 and 3 are quieter, but only core 1 shares core 0's L3.
            const std::vector cores{
                MakeCore(0, 0, 0, 100.0, { 0.0, 0.0 }),
                MakeCore(1, 2, 0, 99.0, { 3.0, 3.0 }),
                MakeCore(2, 4, 1, 90.0, { 0.0, 0.0 }),
                MakeCore(3, 6, 1, 90.0, { 0.0, 0.0 }) };
            const auto selection = SelectCores(cores, AllLps, 0xFF, Config);
            context.Check(selection && CoreIndex(cores, selection->Dominant) == 0 &&
                CoreIndex(cores, selection->Worker) == 1 && selection->Reserved == 0x7,
                L"worker core shares the dominant core's L3");
        }

        void SpeedToleranceDefersToInterruptLoad(TestContext& context)
        {
            const auto dominantFor = [](double secondSpeed)
            {
                const std::vector cores{
                    MakeCore(0, 0, 0, 100.0, { 2.0, 0.0 }),
                    MakeCore(1, 2, 0, secondSpeed, { 0.0, 0.0 }),
                    MakeCore(2, 4, 0, 90.0, { 0.0, 0.0 }) };
                const auto selection = SelectCores(cores, AllLps, 0x3F, Config);
                return selection ? std::optional{ CoreIndex(cores, selection->Dominant) }
                    : std::nullopt;
            };
            context.Check(dominantFor(99.5) == 1u,
                L"a quieter core within the speed tolerance becomes dominant");
            context.Check(dominantFor(98.5) == 0u,
                L"a quieter core outside the speed tolerance does not become dominant");
        }

        void PrefersQuieterLpWithinCore(TestContext& context)
        {
            const std::vector cores{
                MakeCore(0, 0, 0, 100.0, { 1.0, 0.1 }),
                MakeCore(1, 2, 0, 90.0, { 0.1, 1.0 }) };
            const auto selection = SelectCores(cores, AllLps, 0xF, Config);
            context.Check(selection && LpIndex(cores, selection->Dominant) == 1 &&
                LpIndex(cores, selection->Worker) == 2 && selection->Reserved == 0x7,
                L"the quieter logical processor of each core is chosen");
        }

        void RejectsPlansWithoutFreeLp(TestContext& context)
        {
            // Two cores without SMT: reserving both would leave nothing for other programs.
            const std::vector cores{
                MakeCore(0, 0, 0, 100.0, { 0.0 }),
                MakeCore(1, 1, 0, 100.0, { 0.0 }) };
            context.Check(!SelectCores(cores, AllLps, 0x3, Config),
                L"selection fails when no logical processor is left for other programs");
        }

        void RejectsInvalidInput(TestContext& context)
        {
            const std::vector cores{
                MakeCore(0, 0, 0, 100.0, { 0.0, 0.0 }),
                MakeCore(1, 2, 0, 100.0, { 0.0, 0.0 }) };
            constexpr double nan = std::numeric_limits<double>::quiet_NaN();
            context.Check(!SelectCores({}, AllLps, 0xF, Config), L"empty input is rejected");
            context.Check(!SelectCores(cores, 0, 0xF, Config), L"empty game mask is rejected");
            context.Check(!SelectCores(cores, AllLps, 0xF, { .SpeedTolerancePercent = nan }),
                L"non-finite tolerance is rejected");
        }

        // ---- Diagnostic-log fixtures ------------------------------------------------
        // A fixture is a trimmed diagnostic log: the selection inputs plus the logged result.

        struct Fixture
        {
            KAFFINITY GameMask = 0;
            KAFFINITY SystemMask = 0;
            std::vector<CoreMeasurement> Cores;
            std::optional<SelectionConfig> Config;
            std::uint32_t DominantCore = 0;
            std::uint32_t DominantLp = 0;
            std::uint32_t WorkerCore = 0;
            std::uint32_t WorkerLp = 0;
            std::optional<KAFFINITY> Reserved;
        };

        using Tokens = std::vector<std::string_view>;

        [[nodiscard]] Tokens Split(std::string_view text)
        {
            Tokens tokens;
            for (const auto part : text | std::views::split(' '))
                if (!part.empty()) tokens.emplace_back(part.begin(), part.end());
            return tokens;
        }

        // Returns the value of the nth "key=value" token (0-based), e.g. the worker's "core".
        [[nodiscard]] std::optional<std::string_view> Field(const Tokens& tokens,
            std::string_view key, int occurrence = 0)
        {
            for (const std::string_view token : tokens)
            {
                if (token.size() <= key.size() || !token.starts_with(key) ||
                    token[key.size()] != '=')
                    continue;
                if (occurrence-- == 0) return token.substr(key.size() + 1);
            }
            return std::nullopt;
        }

        template <class T>
        [[nodiscard]] std::optional<T> Number(std::optional<std::string_view> text)
        {
            if (!text) return std::nullopt;
            std::string_view value = *text;
            if (value.ends_with('%')) value.remove_suffix(1);
            int base = 10;
            if constexpr (std::is_integral_v<T>)
                if (value.starts_with("0x")) { value.remove_prefix(2); base = 16; }

            T result{};
            std::from_chars_result parsed;
            if constexpr (std::is_integral_v<T>)
                parsed = std::from_chars(value.data(), value.data() + value.size(), result, base);
            else
                parsed = std::from_chars(value.data(), value.data() + value.size(), result);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
                return std::nullopt;
            return result;
        }

        [[nodiscard]] std::expected<Fixture, std::string> ParseFixture(
            const std::filesystem::path& path)
        {
            std::ifstream file(path);
            if (!file) return std::unexpected("cannot open file");

            Fixture fixture;
            bool hasAffinity = false;
            bool hasResult = false;
            std::string line;
            for (int number = 1; std::getline(file, line); ++number)
            {
                if (line.ends_with('\r')) line.pop_back();
                const auto separator = line.find(' ');
                if (separator == std::string::npos) continue;
                const std::string_view message = std::string_view{ line }.substr(separator + 1);
                const Tokens tokens = Split(message);
                const auto fail = [&] { return std::unexpected("line " + std::to_string(number)); };

                if (message.starts_with("game affinity "))
                {
                    const auto game = Number<std::uint64_t>(Field(tokens, "original"));
                    const auto system = Number<std::uint64_t>(Field(tokens, "system"));
                    if (!game || !system) return fail();
                    fixture.GameMask = static_cast<KAFFINITY>(*game);
                    fixture.SystemMask = static_cast<KAFFINITY>(*system);
                    hasAffinity = true;
                }
                else if (message.starts_with("core="))
                {
                    const auto index = Number<std::uint32_t>(Field(tokens, "core"));
                    const auto mask = Number<std::uint64_t>(Field(tokens, "lpMask"));
                    const auto l3 = Number<std::uint32_t>(Field(tokens, "l3"));
                    const auto l3Bytes = Number<std::uint64_t>(Field(tokens, "l3bytes"));
                    const auto speed = Number<double>(Field(tokens, "speed"));
                    const auto efficiency = Number<int>(Field(tokens, "eff"));
                    if (!index || !mask || !l3 || !l3Bytes || !speed || !efficiency) return fail();
                    fixture.Cores.push_back({ .Core = { .Index = *index,
                        .LogicalMask = static_cast<KAFFINITY>(*mask), .L3Index = *l3,
                        .L3SizeBytes = *l3Bytes,
                        .EfficiencyClass = *efficiency < 0 ? std::nullopt
                            : std::optional{ static_cast<std::uint8_t>(*efficiency) } },
                        .RelativeSpeedPercent = *speed });
                }
                else if (message.starts_with("counter lp="))
                {
                    const auto lp = Number<std::uint32_t>(Field(tokens, "lp"));
                    const auto interrupt = Number<double>(Field(tokens, "interrupt"));
                    const auto dpc = Number<double>(Field(tokens, "dpc"));
                    if (!lp || !interrupt || !dpc || fixture.Cores.empty()) return fail();
                    fixture.Cores.back().Lps.push_back(
                        { .Index = *lp, .InterruptDpcPercent = *interrupt + *dpc });
                }
                else if (message.starts_with("selection config "))
                {
                    const auto speed = Number<double>(Field(tokens, "speedTolerance"));
                    const auto interruptDpc = Number<double>(Field(tokens, "intdpcTolerance"));
                    if (!speed || !interruptDpc) return fail();
                    fixture.Config = SelectionConfig{ .SpeedTolerancePercent = *speed,
                        .InterruptDpcTolerancePercent = *interruptDpc };
                }
                else if (message.starts_with("dominant core="))
                {
                    const auto dominantCore = Number<std::uint32_t>(Field(tokens, "core", 0));
                    const auto dominantLp = Number<std::uint32_t>(Field(tokens, "lp", 0));
                    const auto workerCore = Number<std::uint32_t>(Field(tokens, "core", 1));
                    const auto workerLp = Number<std::uint32_t>(Field(tokens, "lp", 1));
                    const auto reserved = Number<std::uint64_t>(Field(tokens, "reserved"));
                    if (!dominantCore || !dominantLp || !workerCore || !workerLp || !reserved)
                        return fail();
                    fixture.DominantCore = *dominantCore;
                    fixture.DominantLp = *dominantLp;
                    fixture.WorkerCore = *workerCore;
                    fixture.WorkerLp = *workerLp;
                    fixture.Reserved = static_cast<KAFFINITY>(*reserved);
                    hasResult = true;
                }
            }

            if (!hasAffinity || fixture.Cores.empty() || !fixture.Config || !hasResult)
                return std::unexpected("missing affinity, core, config or result lines");
            return fixture;
        }

        void CheckFixture(TestContext& context, const std::filesystem::path& path)
        {
            const std::wstring name = L"fixture " + path.filename().wstring();
            const auto fixture = ParseFixture(path);
            if (!fixture)
            {
                std::wcerr << name << L": " << std::wstring(fixture.error().begin(),
                    fixture.error().end()) << L'\n';
                context.Check(false, name + L" parses");
                return;
            }

            const auto selection = SelectCores(fixture->Cores, fixture->GameMask,
                fixture->SystemMask, *fixture->Config);
            context.Check(selection &&
                CoreIndex(fixture->Cores, selection->Dominant) == fixture->DominantCore &&
                LpIndex(fixture->Cores, selection->Dominant) == fixture->DominantLp &&
                CoreIndex(fixture->Cores, selection->Worker) == fixture->WorkerCore &&
                LpIndex(fixture->Cores, selection->Worker) == fixture->WorkerLp &&
                selection->Reserved == *fixture->Reserved,
                name + L" reproduces the logged selection");
        }
    }

    void RunCoreSelectionTests(TestContext& context)
    {
        SkipsL3GroupsWithOneCore(context);
        WorkerSharesDominantL3(context);
        SpeedToleranceDefersToInterruptLoad(context);
        PrefersQuieterLpWithinCore(context);
        RejectsPlansWithoutFreeLp(context);
        RejectsInvalidInput(context);
    }

    void RunCoreSelectionFixtures(TestContext& context, const std::filesystem::path& directory)
    {
        std::error_code error;
        std::vector<std::filesystem::path> files;
        for (const auto& entry : std::filesystem::directory_iterator(directory, error))
            if (entry.is_regular_file() && entry.path().extension() == L".log")
                files.push_back(entry.path());
        std::ranges::sort(files);

        // An empty or missing directory must fail rather than pass silently.
        context.Check(!error && !files.empty(), L"fixture directory contains .log files");
        for (const auto& file : files) CheckFixture(context, file);
    }
}
