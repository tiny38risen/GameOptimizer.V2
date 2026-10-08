using System.Runtime.InteropServices;

namespace GameOptimizer.V2;

internal sealed class NativeEngineSession : IDisposable
{
    private IntPtr handle;

    private NativeEngineSession(IntPtr handle)
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
                // If native recovery state cannot be read, keep the UI in recovery-only mode.
                return true;
            }
        }
    }

    internal static NativeEngineSession? Start(GameTarget target, out NativeEngineStatus status)
    {
        IntPtr sessionHandle = NativeEngineApi.StartSession(
            checked((uint)target.ProcessId),
            checked((ulong)target.ProcessCreationFileTime),
            target.FullPath,
            out status);
        return sessionHandle == IntPtr.Zero ? null : new NativeEngineSession(sessionHandle);
    }

    internal void RequestStop()
    {
        if (handle != IntPtr.Zero)
            NativeEngineApi.RequestStop(handle);
    }

    internal async Task<SessionResult> WaitForCompletionAsync(IProgress<SessionStatus> progress)
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

    internal bool TryRestore()
    {
        if (handle == IntPtr.Zero) return false;
        int restored = NativeEngineApi.TryRestore(handle);
        return restored != 0 && ReadStatus().RecoveryRequired == 0;
    }

    private NativeEngineStatus ReadStatus()
    {
        if (handle == IntPtr.Zero || NativeEngineApi.ReadStatus(handle, out var status) == 0)
            throw new InvalidOperationException("C++ 엔진 상태를 읽지 못했습니다.");
        return status;
    }

    public void Dispose()
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
    internal uint RecoveryRequired;
    internal uint Passed;
    internal uint ProcessId;
    internal uint ThreadId;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 512)]
    internal string Message;

    internal SessionStatus ToSessionStatus() => new(
        HasCandidate != 0, Confirmed != 0, ProcessId, ThreadId, Message ?? string.Empty);

    internal SessionResult ToSessionResult() => new(
        Passed != 0, RecoveryRequired != 0, Message ?? string.Empty,
        HasCandidate != 0, ProcessId, ThreadId);
}

internal sealed record SessionStatus(
    bool HasCandidate, bool Confirmed, uint ProcessId, uint ThreadId, string Message);

internal sealed record SessionResult(
    bool Passed, bool RecoveryRequired, string Message,
    bool HasCandidate, uint ProcessId, uint ThreadId);

internal static class NativeEngineApi
{
    private const string Library = "GameOptimizer.Engine.dll";

    [DllImport(Library, EntryPoint = "go_start_session", ExactSpelling = true,
        CharSet = CharSet.Unicode, CallingConvention = CallingConvention.Cdecl)]
    internal static extern IntPtr StartSession(
        uint processId, ulong processCreationFileTime,
        [MarshalAs(UnmanagedType.LPWStr)] string? fullPath,
        out NativeEngineStatus startupStatus);

    [DllImport(Library, EntryPoint = "go_read_status", ExactSpelling = true,
        CallingConvention = CallingConvention.Cdecl)]
    internal static extern int ReadStatus(IntPtr session, out NativeEngineStatus status);

    [DllImport(Library, EntryPoint = "go_request_stop", ExactSpelling = true,
        CallingConvention = CallingConvention.Cdecl)]
    internal static extern int RequestStop(IntPtr session);

    [DllImport(Library, EntryPoint = "go_wait", ExactSpelling = true,
        CallingConvention = CallingConvention.Cdecl)]
    internal static extern int Wait(IntPtr session, uint timeoutMilliseconds);

    [DllImport(Library, EntryPoint = "go_try_restore", ExactSpelling = true,
        CallingConvention = CallingConvention.Cdecl)]
    internal static extern int TryRestore(IntPtr session);

    [DllImport(Library, EntryPoint = "go_destroy_session", ExactSpelling = true,
        CallingConvention = CallingConvention.Cdecl)]
    internal static extern int DestroySession(IntPtr session);
}
