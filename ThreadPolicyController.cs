using System.Diagnostics;

namespace GameOptimizer.V2;

internal sealed class ThreadPolicyController : IDisposable
{
    private PendingRestore? pending;

    internal bool HasPendingRecovery => pending is { NeedsAffinityRestore: true } or { NeedsPriorityRestore: true };
    internal string? LastError { get; private set; }

    internal ApplyResult Apply(
        GameTarget target,
        Process process,
        ThreadIdentity identity,
        CpuPlacement placement,
        bool raisePriority,
        CancellationToken cancellationToken)
    {
        if (HasPendingRecovery)
            return new(false, true, "이전 후보의 복구가 끝나지 않아 새 스레드에 적용하지 않았습니다.");

        NativeMethods.SafeThreadHandle? thread = null;
        try
        {
            target.Validate(process);
            thread = NativeMethods.OpenThread(
                NativeMethods.ThreadQueryInformation | NativeMethods.ThreadSetInformation | NativeMethods.Synchronize,
                false, identity.ThreadId);
            if (thread.IsInvalid)
                return new(false, false, $"후보 TID {identity.ThreadId}에 접근할 수 없습니다 ({System.Runtime.InteropServices.Marshal.GetLastWin32Error()}).");

            if (!MatchesIdentity(thread, identity))
                return new(false, false, "후보 스레드가 관찰 이후 종료되거나 재사용되었습니다.");
            if (!NativeMethods.GetThreadGroupAffinity(thread, out var originalAffinity))
                return new(false, false, "후보 스레드의 CPU 배치를 읽지 못했습니다.");

            int originalPriority = NativeMethods.GetThreadPriority(thread);
            if (originalPriority == NativeMethods.ThreadPriorityErrorReturn)
                return new(false, false, "후보 스레드의 우선순위를 읽지 못했습니다.");

            var desiredAffinity = placement == CpuPlacement.Keep
                ? originalAffinity
                : new NativeMethods.GroupAffinity
                {
                    Group = originalAffinity.Group,
                    Mask = new UIntPtr(SelectLogicalProcessors(originalAffinity.Mask.ToUInt64(), placement))
                };
            int desiredPriority = raisePriority ? NativeMethods.ThreadPriorityAboveNormal : originalPriority;
            bool changeAffinity = placement != CpuPlacement.Keep &&
                !SameAffinity(originalAffinity, desiredAffinity);
            bool changePriority = raisePriority && originalPriority != desiredPriority;
            if (!changeAffinity && !changePriority)
                return new(true, false, "선택한 설정이 이미 적용되어 있습니다.");

            target.Validate(process);
            if (!MatchesIdentity(thread, identity))
                return new(false, false, "적용 직전 후보 스레드 신원이 달라졌습니다.");
            cancellationToken.ThrowIfCancellationRequested();

            pending = new PendingRestore(
                thread, identity, originalAffinity, originalPriority,
                changeAffinity, desiredAffinity, changePriority, desiredPriority);
            thread = null; // The recovery owner retains the handle until restore is confirmed.

            target.Validate(process);
            cancellationToken.ThrowIfCancellationRequested();
            if (pending.NeedsAffinityRestore &&
                !NativeMethods.SetThreadGroupAffinity(pending.Thread, pending.AppliedAffinity, out _))
                return FailAndAttemptRestore("CPU 배치 적용에 실패했습니다.");

            target.Validate(process);
            cancellationToken.ThrowIfCancellationRequested();
            if (pending.NeedsPriorityRestore &&
                !NativeMethods.SetThreadPriority(pending.Thread, pending.AppliedPriority))
                return FailAndAttemptRestore("후보 우선순위 적용에 실패했습니다.");

            if (!VerifyApplied(pending))
                return FailAndAttemptRestore("적용 후 읽기 검증에 실패했습니다.");

            return new(true, false, "후보 설정을 적용하고 읽기 검증했습니다.");
        }
        catch (OperationCanceledException)
        {
            if (HasPendingRecovery && !TryRestore())
                return new(false, true, "중단 중 원래 설정을 복구하지 못했습니다.");
            throw;
        }
        catch (Exception ex)
        {
            if (HasPendingRecovery && !TryRestore())
                return new(false, true, "실행 중 오류가 발생했고 복구 확인이 필요합니다: " + LastError);
            return new(false, false, ex.Message);
        }
        finally
        {
            thread?.Dispose();
        }
    }

    internal bool TryRestore()
    {
        var state = pending;
        if (state is null)
        {
            LastError = null;
            return true;
        }

        uint wait = NativeMethods.WaitForSingleObject(state.Thread, 0);
        if (wait == NativeMethods.WaitObject0)
        {
            // A terminated thread no longer carries the modified scheduling state.
            ClearPending();
            LastError = null;
            return true;
        }
        if (wait != NativeMethods.WaitTimeout)
        {
            LastError = "후보 스레드의 생존 상태를 확인할 수 없습니다.";
            return false;
        }

        if (!MatchesIdentity(state.Thread, state.Identity))
        {
            LastError = "복구 대상의 TID·PID·생성 시각이 원본과 일치하지 않습니다.";
            return false;
        }

        if (state.NeedsAffinityRestore)
        {
            if (!NativeMethods.GetThreadGroupAffinity(state.Thread, out var current))
            {
                LastError = "복구 전 CPU 배치를 읽지 못했습니다.";
                return false;
            }

            if (SameAffinity(current, state.OriginalAffinity))
            {
                state.NeedsAffinityRestore = false;
            }
            else if (!SameAffinity(current, state.AppliedAffinity))
            {
                LastError = "CPU 배치가 외부에서 변경되어 안전한 원복을 확인할 수 없습니다.";
                return false;
            }
            else
            {
                if (!NativeMethods.SetThreadGroupAffinity(state.Thread, state.OriginalAffinity, out _) ||
                    !NativeMethods.GetThreadGroupAffinity(state.Thread, out var restored) ||
                    !SameAffinity(restored, state.OriginalAffinity))
                {
                    LastError = "원래 CPU 배치를 복구하거나 검증하지 못했습니다.";
                    return false;
                }
                state.NeedsAffinityRestore = false;
            }
        }

        if (state.NeedsPriorityRestore)
        {
            int current = NativeMethods.GetThreadPriority(state.Thread);
            if (current == NativeMethods.ThreadPriorityErrorReturn)
            {
                LastError = "복구 전 우선순위를 읽지 못했습니다.";
                return false;
            }

            if (current == state.OriginalPriority)
            {
                state.NeedsPriorityRestore = false;
            }
            else if (current != state.AppliedPriority)
            {
                LastError = "우선순위가 외부에서 변경되어 안전한 원복을 확인할 수 없습니다.";
                return false;
            }
            else
            {
                if (!NativeMethods.SetThreadPriority(state.Thread, state.OriginalPriority) ||
                    NativeMethods.GetThreadPriority(state.Thread) != state.OriginalPriority)
                {
                    LastError = "원래 우선순위를 복구하거나 검증하지 못했습니다.";
                    return false;
                }
                state.NeedsPriorityRestore = false;
            }
        }

        if (!state.NeedsAffinityRestore && !state.NeedsPriorityRestore)
        {
            ClearPending();
            LastError = null;
            return true;
        }

        LastError = "복구가 일부만 확인되었습니다.";
        return false;
    }

    private ApplyResult FailAndAttemptRestore(string message)
    {
        if (TryRestore()) return new(false, false, message + " 원래 설정은 복구했습니다.");
        return new(false, true, message + " 복구 확인 필요: " + LastError);
    }

    private static bool VerifyApplied(PendingRestore state)
    {
        if (state.NeedsAffinityRestore &&
            (!NativeMethods.GetThreadGroupAffinity(state.Thread, out var affinity) ||
             !SameAffinity(affinity, state.AppliedAffinity)))
            return false;
        return !state.NeedsPriorityRestore ||
            NativeMethods.GetThreadPriority(state.Thread) == state.AppliedPriority;
    }

    private static bool MatchesIdentity(NativeMethods.SafeThreadHandle thread, ThreadIdentity identity)
    {
        if (NativeMethods.GetThreadId(thread) != identity.ThreadId ||
            NativeMethods.GetProcessIdOfThread(thread) != (uint)identity.ProcessId ||
            !NativeMethods.GetThreadTimes(thread, out var creation, out _, out _, out _))
            return false;
        return creation.Value == identity.CreationFileTime;
    }

    private static bool SameAffinity(NativeMethods.GroupAffinity left, NativeMethods.GroupAffinity right) =>
        left.Group == right.Group && left.Mask == right.Mask;

    private static ulong SelectLogicalProcessors(ulong availableMask, CpuPlacement placement)
    {
        int wanted = placement == CpuPlacement.OneLogicalProcessor ? 1 : 2;
        ulong selected = 0;
        for (int bit = 0; bit < 64 && wanted > 0; bit++)
        {
            ulong candidate = 1UL << bit;
            if ((availableMask & candidate) == 0) continue;
            selected |= candidate;
            wanted--;
        }

        if (wanted != 0)
            throw new InvalidOperationException("후보 스레드의 허용 CPU 수가 선택한 배치보다 적습니다.");
        return selected;
    }

    private void ClearPending()
    {
        pending?.Thread.Dispose();
        pending = null;
    }

    public void Dispose()
    {
        if (!HasPendingRecovery)
            ClearPending();
    }

    private sealed class PendingRestore(
        NativeMethods.SafeThreadHandle thread,
        ThreadIdentity identity,
        NativeMethods.GroupAffinity originalAffinity,
        int originalPriority,
        bool needsAffinityRestore,
        NativeMethods.GroupAffinity appliedAffinity,
        bool needsPriorityRestore,
        int appliedPriority)
    {
        internal NativeMethods.SafeThreadHandle Thread { get; } = thread;
        internal ThreadIdentity Identity { get; } = identity;
        internal NativeMethods.GroupAffinity OriginalAffinity { get; } = originalAffinity;
        internal int OriginalPriority { get; } = originalPriority;
        internal bool NeedsAffinityRestore { get; set; } = needsAffinityRestore;
        internal NativeMethods.GroupAffinity AppliedAffinity { get; } = appliedAffinity;
        internal bool NeedsPriorityRestore { get; set; } = needsPriorityRestore;
        internal int AppliedPriority { get; } = appliedPriority;
    }
}

internal sealed record ApplyResult(bool Success, bool RecoveryRequired, string Message);
