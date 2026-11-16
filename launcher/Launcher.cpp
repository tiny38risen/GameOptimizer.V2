#include <Windows.h>

#include <format>
#include <string>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) // app\GameOptimizer.V2.exe 실행 후 종료
{
    std::wstring path(MAX_PATH, L'\0');
    DWORD length = 0;
    while ((length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()))) == path.size())
        path.resize(path.size() * 2);
    if (length == 0) return 1;
    path.resize(length);

    const std::wstring appDirectory = path.substr(0, path.find_last_of(L'\\') + 1) + L"app\\";
    const std::wstring appPath = appDirectory + L"GameOptimizer.V2.exe";

    if (GetFileAttributesW(appPath.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        MessageBoxW(nullptr, L"app 폴더를 찾지 못했습니다.\n압축 파일을 모두 푼 뒤 다시 실행해 주세요.",
            L"게임옵저버", MB_ICONERROR);
        return 1;
    }

    std::wstring commandLine = L"\"" + appPath + L"\"";
    STARTUPINFOW startup{ .cb = sizeof(startup) };
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(appPath.c_str(), commandLine.data(), nullptr, nullptr, FALSE, 0,
            nullptr, appDirectory.c_str(), &startup, &process))
    {
        const std::wstring message = std::format(L"프로그램을 시작하지 못했습니다. (오류 {})", GetLastError());
        MessageBoxW(nullptr, message.c_str(), L"게임옵저버", MB_ICONERROR);
        return 1;
    }

    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return 0;
}
