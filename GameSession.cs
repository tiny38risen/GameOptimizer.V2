using System.Diagnostics;

namespace GameOptimizer.V2;

internal sealed class GameSession : IDisposable
{
    private readonly ThreadCandidateTracker tracker = new();
    private readonly ThreadPolicyController policy = new();
    private ThreadIdentity? lastHandledCandidate;

    internal bool IsRunning { get; private set; }
    internal bool HasPendingRecovery => policy.HasPendingRecovery;

    internal async Task<SessionResult> RunAsync(
        GameTarget target,
        CpuPlacement placement,
        bool raisePriority,
        IProgress<SessionStatus> progress,
        CancellationToken cancellationToken)
    {
        SessionResult result = new(true, false, "추적을 중단했습니다. 원래 설정 복구를 확인했습니다.", null);
        IsRunning = true;
        try
        {
            using var process = target.OpenAndValidate();
            while (true)
            {
                cancellationToken.ThrowIfCancellationRequested();
                target.Validate(process);
                var observation = tracker.Observe(target.ProcessId);
                cancellationToken.ThrowIfCancellationRequested();

                if (observation.Candidate is { } candidate && observation.Confirmed)
                {
                    if (lastHandledCandidate != candidate)
                    {
                        if (policy.HasPendingRecovery && !policy.TryRestore())
                        {
                            result = new(false, true,
                                "이전 후보의 복구를 확인하지 못해 새 후보를 변경하지 않았습니다: " + policy.LastError,
                                candidate);
                            break;
                        }

                        lastHandledCandidate = candidate;
                        if (placement != CpuPlacement.Keep || raisePriority)
                        {
                            target.Validate(process);
                            var applied = policy.Apply(target, process, candidate, placement,
                                raisePriority, cancellationToken);
                            if (!applied.Success)
                            {
                                result = new(false, applied.RecoveryRequired, applied.Message, candidate);
                                break;
                            }
                        }
                    }

                    string message = placement == CpuPlacement.Keep && !raisePriority
                        ? "후보를 추적 중입니다. 설정은 변경하지 않았습니다."
                        : policy.HasPendingRecovery
                            ? "안정 후보의 설정을 적용해 확인했습니다. 중단하면 원복합니다."
                            : "안정 후보가 이미 선택한 설정을 사용 중입니다.";
                    progress.Report(new(candidate, true, message));
                }
                else
                {
                    if (policy.HasPendingRecovery)
                    {
                        if (!policy.TryRestore())
                        {
                            result = new(false, true,
                                "후보 근거가 끊겼고 원래 설정을 복구하지 못했습니다: " + policy.LastError,
                                observation.Candidate);
                            break;
                        }
                        lastHandledCandidate = null;
                    }

                    progress.Report(new(observation.Candidate, false,
                        observation.Candidate is null
                            ? observation.Detail
                            : $"TID {observation.Candidate.ThreadId}의 후보 안정성을 확인 중입니다."));
                }

                await Task.Delay(TimeSpan.FromMilliseconds(900), cancellationToken);
            }
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            result = new(true, false, "중단 요청을 받았습니다. 복구를 확인합니다.", lastHandledCandidate);
        }
        catch (Exception ex)
        {
            result = new(false, false, "실행을 멈췄습니다: " + ex.Message, lastHandledCandidate);
        }
        finally
        {
            IsRunning = false;
            if (policy.HasPendingRecovery && !policy.TryRestore())
                result = result with
                {
                    Passed = false,
                    RecoveryRequired = true,
                    Message = "원래 설정 복구를 확인하지 못했습니다. 재시도 전에는 새 적용을 차단합니다: " + policy.LastError
                };
        }

        if (!policy.HasPendingRecovery && result.RecoveryRequired)
            result = result with
            {
                RecoveryRequired = false,
                Message = "실행은 실패했지만 원래 설정 복구를 확인했습니다. " + result.Message
            };
        if (!policy.HasPendingRecovery && cancellationToken.IsCancellationRequested && result.Passed)
            result = result with { Message = "중단했고 원래 설정 복구를 확인했습니다." };
        return result;
    }

    internal bool TryRestore() => policy.TryRestore();

    public void Dispose() => policy.Dispose();
}
