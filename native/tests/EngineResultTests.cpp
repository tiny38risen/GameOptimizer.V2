#include "TestSupport.h"

#include "../EngineResult.h"

#include <utility>

namespace gameoptimizer::engine::tests
{
    void RunEngineResultTests(TestContext& context)
    {
        SetLastError(5);
        Result<int> win32 = Fail(L"x");
        context.Check(!win32 && win32.error().Is(ErrorKind::Win32) &&
            std::cmp_equal(win32.error().Code, 5),
            L"Fail preserves the Win32 error code");

        constexpr std::uint32_t pdhCode = 0xC0000BB8u;
        Result<int> pdh = FailPdh(L"x", static_cast<LONG>(pdhCode));
        context.Check(!pdh && pdh.error().Is(ErrorKind::Pdh) &&
            std::cmp_equal(pdh.error().Code, pdhCode) &&
            pdh.error().Message.find(L"0xC0000BB8") != std::wstring::npos,
            L"FailPdh preserves the PDH status and formats it");

        Result<int> cancelled = Cancelled();
        context.Check(IsCancelled(cancelled), L"Cancelled is recognized");

        Result<int> targetExited = TargetExited(L"x");
        context.Check(!IsCancelled(targetExited) &&
            targetExited.error().Is(ErrorKind::TargetExited),
            L"TargetExited is distinct from cancellation");

        Result<int> success = 1;
        context.Check(!IsCancelled(success), L"successful results are not cancelled");
    }
}
