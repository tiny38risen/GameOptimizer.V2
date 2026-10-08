using System.Diagnostics;
using System.IO;

namespace GameOptimizer.V2;

internal sealed record GameTarget(
    int ProcessId,
    string Executable,
    string? FullPath,
    string Label,
    long StartTimeUtcTicks)
{
    internal static List<GameTarget> FindVisibleGames()
    {
        var result = new List<GameTarget>();
        foreach (var process in Process.GetProcesses())
        {
            using (process)
            {
                try
                {
                    if (process.Id == Environment.ProcessId || process.MainWindowHandle == IntPtr.Zero ||
                        string.IsNullOrWhiteSpace(process.MainWindowTitle))
                        continue;

                    long start = process.StartTime.ToUniversalTime().Ticks;
                    string executable = process.ProcessName + ".exe";
                    string? path = null;
                    try
                    {
                        string? modulePath = process.MainModule?.FileName;
                        if (!string.IsNullOrWhiteSpace(modulePath))
                        {
                            path = Path.GetFullPath(modulePath);
                            executable = Path.GetFileName(path);
                        }
                    }
                    catch (Exception ex) when (ex is InvalidOperationException or System.ComponentModel.Win32Exception or
                                               NotSupportedException or UnauthorizedAccessException or IOException)
                    {
                        // Keep the visible process by its OS-reported name; PID and creation time remain required.
                    }

                    string pathNote = path is null ? " · 경로 미확인" : string.Empty;
                    result.Add(new(process.Id, executable, path,
                        $"{process.MainWindowTitle} · {executable} ({process.Id}){pathNote}", start));
                }
                catch (Exception ex) when (ex is InvalidOperationException or System.ComponentModel.Win32Exception or
                                           NotSupportedException or UnauthorizedAccessException or IOException)
                {
                    // Processes that cannot be identified are not eligible targets.
                }
            }
        }

        return result.OrderBy(game => game.Label, StringComparer.CurrentCultureIgnoreCase).ToList();
    }

    internal Process OpenAndValidate()
    {
        var process = Process.GetProcessById(ProcessId);
        try
        {
            Validate(process);
            _ = process.Handle; // Keep the selected process object alive for the session.
            return process;
        }
        catch
        {
            process.Dispose();
            throw;
        }
    }

    internal void Validate(Process process)
    {
        if (process.HasExited || process.Id != ProcessId ||
            process.StartTime.ToUniversalTime().Ticks != StartTimeUtcTicks)
            throw new InvalidOperationException("선택한 게임 프로세스가 종료되었거나 교체되었습니다.");

        if (FullPath is not null)
        {
            string currentPath = Path.GetFullPath(process.MainModule?.FileName
                ?? throw new InvalidOperationException("게임 경로를 다시 확인할 수 없습니다."));
            if (!string.Equals(currentPath, FullPath, StringComparison.OrdinalIgnoreCase))
                throw new InvalidOperationException("선택한 PID의 실행 파일이 변경되었습니다.");
        }
    }
}
