using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace GameOptimizer.V2;

internal static class NativeMethods
{
    internal const uint SnapshotThreads = 0x00000004;
    internal const uint ThreadQueryInformation = 0x0040;
    internal const uint ThreadSetInformation = 0x0020;
    internal const uint Synchronize = 0x00100000;
    internal const uint WaitObject0 = 0;
    internal const uint WaitTimeout = 258;
    internal const int ThreadPriorityAboveNormal = 1;
    internal const int ThreadPriorityErrorReturn = 0x7fffffff;

    [StructLayout(LayoutKind.Sequential)]
    internal struct ThreadEntry
    {
        internal uint Size;
        internal uint Usage;
        internal uint ThreadId;
        internal uint OwnerProcessId;
        internal int BasePriority;
        internal int DeltaPriority;
        internal uint Flags;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct FileTime
    {
        internal uint Low;
        internal uint High;
        internal readonly ulong Value => ((ulong)High << 32) | Low;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct GroupAffinity
    {
        internal UIntPtr Mask;
        internal ushort Group;
        internal ushort Reserved0;
        internal ushort Reserved1;
        internal ushort Reserved2;
    }

    internal sealed class SafeThreadHandle : SafeHandleZeroOrMinusOneIsInvalid
    {
        private SafeThreadHandle() : base(true) { }
        protected override bool ReleaseHandle() => CloseHandle(handle);
    }

    internal sealed class SafeSnapshotHandle : SafeHandleZeroOrMinusOneIsInvalid
    {
        private SafeSnapshotHandle() : base(true) { }
        protected override bool ReleaseHandle() => CloseHandle(handle);
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    internal static extern SafeSnapshotHandle CreateToolhelp32Snapshot(uint flags, uint processId);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    internal static extern bool Thread32First(SafeSnapshotHandle snapshot, ref ThreadEntry entry);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    internal static extern bool Thread32Next(SafeSnapshotHandle snapshot, ref ThreadEntry entry);

    [DllImport("kernel32.dll", SetLastError = true)]
    internal static extern SafeThreadHandle OpenThread(uint desiredAccess, bool inheritHandle, uint threadId);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    internal static extern bool GetThreadTimes(
        SafeThreadHandle thread, out FileTime creation, out FileTime exit,
        out FileTime kernel, out FileTime user);

    [DllImport("kernel32.dll", SetLastError = true)]
    internal static extern uint GetThreadId(SafeThreadHandle thread);

    [DllImport("kernel32.dll", SetLastError = true)]
    internal static extern uint GetProcessIdOfThread(SafeThreadHandle thread);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    internal static extern bool GetThreadGroupAffinity(SafeThreadHandle thread, out GroupAffinity affinity);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    internal static extern bool SetThreadGroupAffinity(
        SafeThreadHandle thread, in GroupAffinity affinity, out GroupAffinity previousAffinity);

    [DllImport("kernel32.dll", SetLastError = true)]
    internal static extern int GetThreadPriority(SafeThreadHandle thread);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    internal static extern bool SetThreadPriority(SafeThreadHandle thread, int priority);

    [DllImport("kernel32.dll", SetLastError = true)]
    internal static extern uint WaitForSingleObject(SafeThreadHandle handle, uint milliseconds);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CloseHandle(IntPtr handle);
}

internal enum CpuPlacement
{
    Keep,
    TwoLogicalProcessors,
    OneLogicalProcessor
}

internal sealed record ThreadIdentity(int ProcessId, uint ThreadId, ulong CreationFileTime);
internal sealed record ThreadCandidateObservation(ThreadIdentity? Candidate, bool Confirmed, ulong CpuDelta, string Detail);
internal sealed record SessionStatus(ThreadIdentity? Candidate, bool Confirmed, string Message);
internal sealed record SessionResult(bool Passed, bool RecoveryRequired, string Message, ThreadIdentity? Candidate);
