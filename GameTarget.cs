using System.Diagnostics;
using System.IO;

namespace GameOptimizer.V2;

internal sealed record GameTarget(
    int ProcessId,
    string Executable,
    string? FullPath,
    string Label,
    long ProcessCreationFileTime)
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

                    long creation = process.StartTime.ToUniversalTime().ToFileTimeUtc();
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
                        $"{process.MainWindowTitle} · {executable} ({process.Id}){pathNote}", creation));
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

}
