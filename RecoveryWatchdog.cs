using System.Diagnostics;
using System.ComponentModel;
using System.IO;

namespace GameOptimizer.V2;

internal static class RecoveryWatchdog
{
    private const string Switch = "--watchdog";

    private static string DirectoryPath => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "GameOptimizer.V2", "recovery");

    internal static string Start()
    {
        Directory.CreateDirectory(DirectoryPath);
        using var owner = Process.GetCurrentProcess();
        long creation = owner.StartTime.ToUniversalTime().ToFileTimeUtc();
        string path = Path.Combine(DirectoryPath,
            $"{owner.Id}_{creation}_{Guid.NewGuid():N}.log");
        string readyPath = path + ".ready";
        File.WriteAllBytes(path, []);

        try
        {
            var start = new ProcessStartInfo(Environment.ProcessPath
                ?? throw new InvalidOperationException("실행 파일 경로를 찾지 못했습니다."))
            {
                UseShellExecute = false,
                CreateNoWindow = true,
                WindowStyle = ProcessWindowStyle.Hidden
            };
            start.ArgumentList.Add(Switch);
            start.ArgumentList.Add(owner.Id.ToString());
            start.ArgumentList.Add(creation.ToString());
            start.ArgumentList.Add(path);
            start.ArgumentList.Add(readyPath);

            using var watcher = Process.Start(start)
                ?? throw new InvalidOperationException("복구 감시 프로세스를 시작하지 못했습니다.");
            var deadline = Stopwatch.StartNew();
            while (deadline.Elapsed < TimeSpan.FromSeconds(5))
            {
                if (File.Exists(readyPath))
                {
                    File.Delete(readyPath);
                    return path;
                }
                if (watcher.HasExited) break;
                Thread.Sleep(25);
            }
            throw new InvalidOperationException("복구 감시 프로세스가 준비되지 않았습니다.");
        }
        catch
        {
            Cancel(path);
            throw;
        }
    }

    internal static void Cancel(string path)
    {
        try
        {
            File.Delete(path);
            File.Delete(path + ".ready");
        }
        catch (IOException) { }
    }

    internal static void Run(string[] args)
    {
        if (args.Length != 5 || !int.TryParse(args[1], out int ownerId) ||
            !long.TryParse(args[2], out long creation))
            return;

        string path = args[3];
        string readyPath = args[4];
        try
        {
            using var owner = Process.GetProcessById(ownerId);
            if (owner.StartTime.ToUniversalTime().ToFileTimeUtc() != creation)
            {
                if (File.Exists(path)) Restore(path);
                return;
            }

            File.WriteAllBytes(readyPath, []);
            while (File.Exists(path) && !owner.WaitForExit(500)) { }
            if (File.Exists(path)) Restore(path);
        }
        catch (ArgumentException)
        {
            if (File.Exists(path)) Restore(path);
        }
        catch
        {
            // The journal remains available for recovery at the next launch.
        }
        finally
        {
            try { File.Delete(readyPath); } catch (IOException) { }
        }
    }

    internal static void RestoreStaleJournals()
    {
        if (!Directory.Exists(DirectoryPath)) return;
        foreach (string path in Directory.EnumerateFiles(DirectoryPath, "*.log"))
        {
            string[] parts = Path.GetFileNameWithoutExtension(path).Split('_');
            if (parts.Length != 3 || !int.TryParse(parts[0], out int id) ||
                !long.TryParse(parts[1], out long creation) ||
                !Guid.TryParseExact(parts[2], "N", out _))
                continue;

            try
            {
                using var owner = Process.GetProcessById(id);
                if (owner.StartTime.ToUniversalTime().ToFileTimeUtc() == creation &&
                    !owner.HasExited)
                    continue;
            }
            catch (ArgumentException) { }
            catch (InvalidOperationException) { }
            catch (Win32Exception) { continue; }
            Restore(path);
        }
    }

    private static void Restore(string path)
    {
        using var mutex = new Mutex(false, @"Local\GameOptimizerV2Recovery");
        bool acquired = false;
        try
        {
            try { acquired = mutex.WaitOne(TimeSpan.FromSeconds(10)); }
            catch (AbandonedMutexException) { acquired = true; }
            if (!acquired) return;

            for (int attempt = 0; attempt < 10 && File.Exists(path); attempt++)
            {
                if (NativeEngineApi.RestoreJournal(path) != 0) break;
                Thread.Sleep(500);
            }
        }
        finally
        {
            if (acquired) mutex.ReleaseMutex();
        }
    }
}
