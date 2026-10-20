using System.Runtime.InteropServices;

namespace GameOptimizer.V2;

internal sealed class NativeEngineSession : IDisposable
{
    private IntPtr handle;

    private NativeEngineSession(IntPtr handle) // 네이티브 세션 핸들 보관
    {
        this.handle = handle;
    }

    internal bool HasPendingRecovery
    {
        get
        {
            try
            {
                return ReadStatus().RecoveryRequired != 0;
            }
            catch
            {
                // 상태 조회 실패 시 복구 전용 상태 유지
                return true;
            }
        }
    }

    internal static NativeEngineSession? Start(GameTarget target, out NativeEngineStatus status) // 선택 게임의 네이티브 최적화 세션 시작
    {
        string journalPath = RecoveryWatchdog.Start();
        try
        {
            IntPtr sessionHandle = NativeEngineApi.StartSession(
                checked((uint)target.ProcessId),
                checked((ulong)target.ProcessCreationFileTime),
                target.PathAvailable ? 1U : 0U,
                journalPath,
                out status);
            if (sessionHandle != IntPtr.Zero)
                return new NativeEngineSession(sessionHandle);
            RecoveryWatchdog.Cancel(journalPath);
            return null;
        }
        catch
        {
            RecoveryWatchdog.Cancel(journalPath);
            throw;
        }
    }

    internal void RequestStop() // 네이티브 세션 중단·원복 요청
    {
        if (handle != IntPtr.Zero)
            NativeEngineApi.RequestStop(handle);
    }

    internal async Task<SessionResult> WaitForCompletionAsync(IProgress<SessionStatus> progress) // 세션 완료 대기·주기 상태 UI 전달
    {
        await Task.Run(() =>
        {
            while (true)
            {
                int wait = NativeEngineApi.Wait(handle, 100);
                if (wait < 0)
                    throw new InvalidOperationException("C++ 엔진 실행 상태를 읽지 못했습니다.");

                progress.Report(ReadStatus().ToSessionStatus());
                if (wait == 1) break;
            }
        });

        return ReadStatus().ToSessionResult();
    }

    internal bool TryRestore() // 미완료 설정 원복 재시도
    {
        if (handle == IntPtr.Zero) return false;
        int restored = NativeEngineApi.TryRestore(handle);
        return restored != 0 && ReadStatus().RecoveryRequired == 0;
    }

    private NativeEngineStatus ReadStatus() // 네이티브 세션 상태 조회
    {
        if (handle == IntPtr.Zero || NativeEngineApi.ReadStatus(handle, out var status) == 0)
            throw new InvalidOperationException("C++ 엔진 상태를 읽지 못했습니다.");
        return status;
    }

    public void Dispose() // 복구 확인 후 네이티브 세션 핸들 해제
    {
        if (handle == IntPtr.Zero || HasPendingRecovery) return;
        if (NativeEngineApi.DestroySession(handle) != 0)
            handle = IntPtr.Zero;
    }
}

[StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
internal struct NativeEngineStatus
{
    internal uint HasCandidate;
    internal uint Confirmed;
    internal uint Applied;
    internal uint RecoveryRequired;
    internal uint Passed;
    internal uint ProcessId;
    internal uint ThreadId;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 512)]
    internal string Message;

    internal SessionStatus ToSessionStatus() // 네이티브 상태의 진행 UI 모델 변환
        => new(
        HasCandidate != 0, Confirmed != 0, Applied != 0,
        ProcessId, ThreadId, Message ?? string.Empty);

    internal SessionResult ToSessionResult() // 네이티브 상태의 완료 결과 모델 변환
        => new(
        Passed != 0, RecoveryRequired != 0, Message ?? string.Empty,
        HasCandidate != 0, ProcessId, ThreadId);
}

internal sealed record SessionStatus(
    bool HasCandidate, bool Confirmed, bool Applied,
    uint ProcessId, uint ThreadId, string Message);

internal sealed record SessionResult(
    bool Passed, bool RecoveryRequired, string Message,
    bool HasCandidate, uint ProcessId, uint ThreadId);

[StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
internal struct NativeGameTarget
{
    internal uint ProcessId;
    internal ulong ProcessCreationFileTime;
    internal uint PathAvailable;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 512)]
    internal string? WindowTitle;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)]
    internal string? Executable;

    internal GameTarget ToGameTarget() // 네이티브 창 대상의 UI 모델 변환
        => new(
        checked((int)ProcessId),
        Executable ?? string.Empty,
        WindowTitle ?? string.Empty,
        PathAvailable != 0,
        checked((long)ProcessCreationFileTime));
}

internal static class NativeEngineApi
{
    private const string Library = "GameOptimizer.Engine.dll";

    internal static List<GameTarget> FindVisibleGames() // 네이티브 대상 창 목록 조회·정렬
    {
        if (ListVisibleGames(null, 0, out var requiredCount) == 0)
            throw new InvalidOperationException("C++ 엔진에서 창 목록을 읽지 못했습니다.");

        while (true)
        {
            if (requiredCount > int.MaxValue)
                throw new InvalidOperationException("C++ 엔진의 창 목록 크기가 지원 범위를 넘었습니다.");

            var targets = new NativeGameTarget[checked((int)requiredCount)];
            if (ListVisibleGames(targets, checked((uint)targets.Length), out var actualCount) == 0)
                throw new InvalidOperationException("C++ 엔진에서 창 목록을 읽지 못했습니다.");

            if (actualCount > (uint)targets.Length)
            {
                requiredCount = actualCount;
                continue;
            }

            return targets.Take(checked((int)actualCount))
                .Select(static target => target.ToGameTarget())
                .OrderBy(static target => target.Label, StringComparer.CurrentCultureIgnoreCase)
                .ToList();
        }
    }

    [DllImport(Library, EntryPoint = "go_list_visible_games", ExactSpelling = true,
        CallingConvention = CallingConvention.Cdecl)]
    private static extern int ListVisibleGames(
        [Out, MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 1)] NativeGameTarget[]? targets,
        uint capacity,
        out uint requiredCount); // 창 목록 크기 조회·대상 배열 기록

    [DllImport(Library, EntryPoint = "go_start_session", ExactSpelling = true,
        CharSet = CharSet.Unicode, CallingConvention = CallingConvention.Cdecl)]
    internal static extern IntPtr StartSession(
        uint processId, ulong processCreationFileTime, uint requirePathVerification,
        [MarshalAs(UnmanagedType.LPWStr)] string journalPath,
        out NativeEngineStatus startupStatus); // PID 기반 네이티브 최적화 세션 생성

    [DllImport(Library, EntryPoint = "go_restore_journal", ExactSpelling = true,
        CharSet = CharSet.Unicode, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int RestoreJournal([MarshalAs(UnmanagedType.LPWStr)] string journalPath);

    [DllImport(Library, EntryPoint = "go_read_status", ExactSpelling = true,
        CallingConvention = CallingConvention.Cdecl)]
    internal static extern int ReadStatus(IntPtr session, out NativeEngineStatus status); // 세션 상태·복구 여부 조회

    [DllImport(Library, EntryPoint = "go_request_stop", ExactSpelling = true,
        CallingConvention = CallingConvention.Cdecl)]
    internal static extern int RequestStop(IntPtr session); // 세션 중단·저장 설정 원복 요청

    [DllImport(Library, EntryPoint = "go_wait", ExactSpelling = true,
        CallingConvention = CallingConvention.Cdecl)]
    internal static extern int Wait(IntPtr session, uint timeoutMilliseconds); // 제한 시간 내 세션 완료 대기

    [DllImport(Library, EntryPoint = "go_try_restore", ExactSpelling = true,
        CallingConvention = CallingConvention.Cdecl)]
    internal static extern int TryRestore(IntPtr session); // 미완료 원복 재시도

    [DllImport(Library, EntryPoint = "go_destroy_session", ExactSpelling = true,
        CallingConvention = CallingConvention.Cdecl)]
    internal static extern int DestroySession(IntPtr session); // 복구된 네이티브 세션 해제
}
