using System.Diagnostics;
using System.Runtime.InteropServices;

namespace GameOptimizer.V2;

internal sealed class ThreadCandidateTracker
{
    private static readonly TimeSpan ConfirmationTime = TimeSpan.FromSeconds(4);
    private const int MinimumWindows = 4;
    private readonly Dictionary<ThreadIdentity, ulong> previousCpu = new();
    private ThreadIdentity? currentCandidate;
    private long candidateSince;
    private int candidateWindows;

    internal ThreadCandidateObservation Observe(int processId)
    {
        var snapshot = NativeMethods.CreateToolhelp32Snapshot(NativeMethods.SnapshotThreads, 0);
        if (snapshot.IsInvalid)
        {
            Reset();
            return new(null, false, 0, $"스레드 목록 조회 실패 ({Marshal.GetLastWin32Error()}).");
        }

        var currentCpu = new Dictionary<ThreadIdentity, ulong>();
        bool complete = true;
        var entry = new NativeMethods.ThreadEntry { Size = (uint)Marshal.SizeOf<NativeMethods.ThreadEntry>() };
        bool hasEntry = NativeMethods.Thread32First(snapshot, ref entry);
        if (!hasEntry && Marshal.GetLastWin32Error() != 18)
            complete = false;

        while (hasEntry)
        {
            if (entry.OwnerProcessId == (uint)processId)
            {
                using var thread = NativeMethods.OpenThread(
                    NativeMethods.ThreadQueryInformation | NativeMethods.Synchronize,
                    false, entry.ThreadId);
                if (thread.IsInvalid ||
                    NativeMethods.GetProcessIdOfThread(thread) != (uint)processId ||
                    !NativeMethods.GetThreadTimes(thread, out var creation, out _, out var kernel, out var user))
                {
                    complete = false;
                }
                else
                {
                    var identity = new ThreadIdentity(processId, entry.ThreadId, creation.Value);
                    ulong total = kernel.Value + user.Value;
                    currentCpu[identity] = total;
                }
            }

            entry.Size = (uint)Marshal.SizeOf<NativeMethods.ThreadEntry>();
            hasEntry = NativeMethods.Thread32Next(snapshot, ref entry);
            if (!hasEntry && Marshal.GetLastWin32Error() != 18)
                complete = false;
        }

        if (!complete)
        {
            previousCpu.Clear();
            Reset();
            return new(null, false, 0, "일부 스레드 신원을 확인할 수 없어 후보를 다시 관찰합니다.");
        }

        var deltas = currentCpu
            .Select(pair => new
            {
                pair.Key,
                Delta = previousCpu.TryGetValue(pair.Key, out ulong before) && pair.Value >= before
                    ? pair.Value - before : 0UL
            })
            .OrderByDescending(item => item.Delta)
            .ToList();

        previousCpu.Clear();
        foreach (var pair in currentCpu)
            previousCpu[pair.Key] = pair.Value;

        if (deltas.Count == 0 || deltas[0].Delta == 0)
        {
            Reset();
            return new(null, false, 0, "CPU 사용 표본이 아직 없습니다.");
        }

        var selected = deltas[0];
        long now = Stopwatch.GetTimestamp();
        if (currentCandidate != selected.Key)
        {
            currentCandidate = selected.Key;
            candidateSince = now;
            candidateWindows = 1;
        }
        else
        {
            candidateWindows++;
        }

        bool confirmed = candidateWindows >= MinimumWindows &&
            Stopwatch.GetElapsedTime(candidateSince, now) >= ConfirmationTime;
        string detail = confirmed ? "안정 후보" : "안정성 확인 중";
        return new(selected.Key, confirmed, selected.Delta, detail);
    }

    private void Reset()
    {
        currentCandidate = null;
        candidateSince = 0;
        candidateWindows = 0;
    }
}
