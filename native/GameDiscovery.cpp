#include "GameDiscovery.h"

#include "EngineInternals.h"

#include <TlHelp32.h>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace gameoptimizer::engine
{
    namespace
    {
        struct EnumerationContext
        {
            std::unordered_map<DWORD, std::wstring> Executables;
            std::unordered_set<DWORD> SeenProcesses;
            std::vector<GO_GAME_TARGET> Targets;
            bool Failed = false;
        };

        std::unordered_map<DWORD, std::wstring> ReadExecutableNames() // 프로세스 스냅샷의 PID별 실행 파일명 조회
        {
            std::unordered_map<DWORD, std::wstring> names;
            UniqueHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
            if (!snapshot.IsValid()) return names;

            PROCESSENTRY32W entry{};
            entry.dwSize = sizeof(entry);
            if (!Process32FirstW(snapshot.Get(), &entry)) return names;

            do
            {
                names.emplace(entry.th32ProcessID, entry.szExeFile);
                entry.dwSize = sizeof(entry);
            } while (Process32NextW(snapshot.Get(), &entry));

            return names;
        }

        std::wstring FileNameFromPath(const std::wstring& path) // 실행 경로에서 파일명 추출
        {
            const auto separator = path.find_last_of(L"\\/");
            return separator == std::wstring::npos ? path : path.substr(separator + 1);
        }

        template <std::size_t Capacity>
        void CopyText(wchar_t (&destination)[Capacity], const std::wstring& source) noexcept // C ABI 배열에 문자열 안전 복사
        {
            wcsncpy_s(destination, Capacity, source.c_str(), _TRUNCATE);
        }

        BOOL CALLBACK AddVisibleWindow(HWND window, LPARAM parameter) noexcept // 보이는 창의 PID·제목·실행 경로 수집
        {
            auto& context = *reinterpret_cast<EnumerationContext*>(parameter);
            try
            {
                if (!IsWindowVisible(window) || GetWindow(window, GW_OWNER) != nullptr)
                    return TRUE;

                DWORD processId = 0;
                if (GetWindowThreadProcessId(window, &processId) == 0 || processId == 0 ||
                    processId == GetCurrentProcessId() || context.SeenProcesses.contains(processId))
                    return TRUE;

                const int titleLength = GetWindowTextLengthW(window);
                if (titleLength <= 0) return TRUE;

                const std::size_t titleCapacity = std::min<std::size_t>(
                    static_cast<std::size_t>(titleLength), GO_WINDOW_TITLE_CAPACITY - 1) + 1;
                std::vector<wchar_t> title(titleCapacity);
                const int copiedLength = GetWindowTextW(window, title.data(), static_cast<int>(title.size()));
                if (copiedLength <= 0) return TRUE;

                UniqueHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId));
                if (!process.IsValid()) return TRUE;

                FILETIME creation{}, exit{}, kernel{}, user{};
                if (!GetProcessTimes(process.Get(), &creation, &exit, &kernel, &user)) return TRUE;

                const std::wstring path = ProcessImagePath(process.Get());
                std::wstring executable = path.empty() ? std::wstring{} : FileNameFromPath(path);
                if (executable.empty())
                {
                    const auto fallback = context.Executables.find(processId);
                    executable = fallback == context.Executables.end()
                        ? L"Unknown.exe"
                        : fallback->second;
                }

                GO_GAME_TARGET target{};
                target.ProcessId = processId;
                target.ProcessCreationFileTime = FileTimeValue(creation);
                target.PathAvailable = path.empty() ? 0U : 1U;
                CopyText(target.WindowTitle, std::wstring(title.data(), copiedLength));
                CopyText(target.Executable, executable);

                context.Targets.push_back(target);
                context.SeenProcesses.insert(processId);
                return TRUE;
            }
            catch (...)
            {
                context.Failed = true;
                return FALSE;
            }
        }
    }

    std::vector<GO_GAME_TARGET> FindVisibleGames() // 보이는 앱 대상 창 목록 조회
    {
        EnumerationContext context;
        context.Executables = ReadExecutableNames();

        if (!EnumWindows(AddVisibleWindow, reinterpret_cast<LPARAM>(&context)) || context.Failed)
            throw std::runtime_error("EnumWindows failed while finding visible game windows.");

        return context.Targets;
    }
}
