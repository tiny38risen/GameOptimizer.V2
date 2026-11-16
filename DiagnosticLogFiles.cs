using System.Globalization;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Text;

namespace GameOptimizer.V2;

internal static class DiagnosticLogFiles
{
    private const int KeepCount = 20;

#if DEBUG
    private const string BuildConfiguration = "Debug";
#else
    private const string BuildConfiguration = "Release";
#endif

    private static readonly UTF8Encoding Utf8WithoutBom = new(encoderShouldEmitUTF8Identifier: false);

    private static string DirectoryPath => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "GameOptimizer.V2", "diagnostics");

    internal static string? Create(GameTarget target) // 진단 로그 파일 생성·첫 줄에 빌드 정보 기록
    {
        try
        {
            Directory.CreateDirectory(DirectoryPath);
            Prune();
            string path = Path.Combine(DirectoryPath,
                $"{target.ProcessId}_{target.ProcessCreationFileTime}_{DateTime.Now:yyyyMMdd_HHmmss}.txt");

            // 엔진은 같은 파일 끝에 이어서 기록하므로 빌드 정보가 항상 첫 줄이 된다.
            File.WriteAllText(path, BuildHeaderLine(), Utf8WithoutBom);
            return path;
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException)
        {
            return null;
        }
    }

    private static string BuildHeaderLine() // 예: 2026-11-16T21:35:38.007 build version=3.0.0-beta1+a9794b0 configuration=Release
    {
        string timestamp = DateTime.Now.ToString("yyyy-MM-dd'T'HH:mm:ss.fff", CultureInfo.InvariantCulture);
        return $"{timestamp} build version={AppVersion()} configuration={BuildConfiguration}\r\n";
    }

    private static string AppVersion() // 버전과 커밋 해시 7자리
    {
        string version = typeof(DiagnosticLogFiles).Assembly
            .GetCustomAttribute<AssemblyInformationalVersionAttribute>()?
            .InformationalVersion ?? "unknown";

        const int CommitLength = 7;
        int separator = version.IndexOf('+');
        return separator >= 0 && version.Length > separator + 1 + CommitLength
            ? version[..(separator + 1 + CommitLength)]
            : version;
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
