#include "..\EngineResult.h"

#include <iostream>
#include <string_view>
#include <utility>

using namespace gameoptimizer::engine;

int wmain()
{
    int failures = 0;
    const auto check = [&failures](bool passed, std::wstring_view name)
    {
        if (!passed)
        {
            ++failures;
            std::wcerr << L"FAIL: " << name << L'\n';
        }
    };

    SetLastError(5);
    Result<int> win32 = Fail(L"x");
    check(!win32 && win32.error().Is(ErrorKind::Win32) && std::cmp_equal(win32.error().Code, 5),
        L"Fail preserves the Win32 error code");

    constexpr std::uint32_t pdhCode = 0xC0000BB8u;
    Result<int> pdh = FailPdh(L"x", static_cast<LONG>(pdhCode));
    check(!pdh && pdh.error().Is(ErrorKind::Pdh) && std::cmp_equal(pdh.error().Code, pdhCode) &&
        pdh.error().Message.find(L"0xC0000BB8") != std::wstring::npos,
        L"FailPdh preserves the PDH status and formats it");

    Result<int> cancelled = Cancelled();
    check(IsCancelled(cancelled), L"Cancelled is recognized");

    Result<int> targetExited = TargetExited(L"x");
    check(!IsCancelled(targetExited) && targetExited.error().Is(ErrorKind::TargetExited),
        L"TargetExited is distinct from cancellation");

    Result<int> success = 1;
    check(!IsCancelled(success), L"successful results are not cancelled");

    return failures == 0 ? 0 : 1;
}
