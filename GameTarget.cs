namespace GameOptimizer.V2;

// 네이티브 창 탐색 결과를 화면에 전달하는 DTO다.
internal sealed record GameTarget(
    int ProcessId,
    string Executable,
    string WindowTitle,
    bool PathAvailable,
    long ProcessCreationFileTime)
{
    public string Label // 창 제목·프로세스·PID 표시 문자열
        =>
        $"{WindowTitle} · {Executable} ({ProcessId})" + (PathAvailable ? string.Empty : " · 경로 미확인");
}
