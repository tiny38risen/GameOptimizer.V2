#include "TestSupport.h"

#include <filesystem>
#include <iostream>

using namespace gameoptimizer::engine::tests;

// Usage: GameOptimizer.Engine.Tests.exe [fixture-directory]
int wmain(int argc, wchar_t** argv)
{
    TestContext context;
    RunEngineResultTests(context);
    RunCoreSelectionTests(context);
    if (argc > 1) RunCoreSelectionFixtures(context, std::filesystem::path{ argv[1] });

    std::wcout << context.Checks() << L" checks, " << context.Failures() << L" failed\n";
    return context.Failures() == 0 ? 0 : 1;
}
