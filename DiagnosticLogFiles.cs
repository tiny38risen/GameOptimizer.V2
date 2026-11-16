using System.IO;
using System.Linq;

namespace GameOptimizer.V2;

internal static class DiagnosticLogFiles
{
    private const int KeepCount = 20;

    private static string DirectoryPath => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "GameOptimizer.V2", "diagnostics");

    internal static string? Create(GameTarget target)
    {
        try
        {
            Directory.CreateDirectory(DirectoryPath);
            Prune();
            return Path.Combine(DirectoryPath,
                $"{target.ProcessId}_{target.ProcessCreationFileTime}_{DateTime.Now:yyyyMMdd_HHmmss}.txt");
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException)
        {
            return null;
        }
    }

    private static void Prune()
    {
        foreach (FileInfo file in new DirectoryInfo(DirectoryPath).EnumerateFiles("*.txt")
            .OrderByDescending(file => file.CreationTimeUtc)
            .Skip(KeepCount - 1))
        {
            try { file.Delete(); }
            catch (Exception exception) when (exception is IOException or UnauthorizedAccessException) { }
        }
    }
}
