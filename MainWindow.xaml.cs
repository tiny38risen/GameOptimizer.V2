using System.ComponentModel;
using System.Windows;

namespace GameOptimizer.V2;

public partial class MainWindow : Window
{
    private NativeEngineSession? session;
    private bool closeWhenStopped;
    private bool allowClose;
    private bool busy;
    private bool stopRequested;

    public MainWindow() // UI 초기화·창 목록 조회·대기 버튼 설정
    {
        InitializeComponent();
        SetButtonState(ButtonState.Idle);
        Loaded += async (_, _) => await RefreshGamesAsync();
    }

    private async Task<bool> RefreshGamesAsync(bool duringStart = false) // 보이는 창 탐색·대상 목록 갱신
    {
        if (busy && !duringStart) return false;
        var selected = GameBox.SelectedItem as GameTarget;
        try
        {
            var targets = await Task.Run(NativeEngineApi.FindVisibleGames);

            GameBox.ItemsSource = targets;
            GameBox.SelectedItem = targets.FirstOrDefault(target => selected is { } previous &&
                target.ProcessId == previous.ProcessId &&
                target.ProcessCreationFileTime == previous.ProcessCreationFileTime)
                ?? (targets.Count == 1 ? targets[0] : null);

            if (targets.Count == 0)
            {
                ShowStatus("보이는 창 없음");
                SetButtonState(ButtonState.Idle);
            }
            else if (targets.Count > 1 && GameBox.SelectedItem is null)
            {
                ShowStatus("대상 창 선택");
                SetButtonState(ButtonState.Idle);
            }
            else
            {
                ShowStatus("대기 중");
                SetButtonState(ButtonState.Idle);
            }

            return true;
        }
        catch (Exception ex)
        {
            ShowStatus("목록 확인 오류", ex.Message);
            SetButtonState(ButtonState.Error);
            return false;
        }
    }

    private async void GameBox_DropDownOpened(object? sender, EventArgs e) // 창 목록 펼칠 때 대상 재검색
        => await RefreshGamesAsync();

    private async void Run_Click(object sender, RoutedEventArgs e) // 단일 버튼으로 최적화 시작·중단·복구
    {
        if (busy)
        {
            if (stopRequested) return;
            stopRequested = true;
            SetButtonState(ButtonState.Recovering);
            ShowStatus("중단 및 복구 중");
            session?.RequestStop();
            RunButton.IsEnabled = false;
            return;
        }

        if (RunButton.Tag as string == ButtonState.Error.ToString() &&
            session?.HasPendingRecovery != true)
        {
            SetButtonState(ButtonState.Idle);
            ShowStatus("대기 중");
            SetControlsEnabled(true);
            return;
        }

        if (session?.HasPendingRecovery == true)
        {
            busy = true;
            stopRequested = true;
            SetControlsEnabled(false);
            SetButtonState(ButtonState.Recovering);
            ShowStatus("원래 설정 복구 중");
            var recoverySession = session;
            bool restored;
            string? restoreError = null;
            try
            {
                restored = await Task.Run(recoverySession.TryRestore);
            }
            catch (Exception ex)
            {
                restored = false;
                restoreError = ex.Message;
            }

            busy = false;
            stopRequested = false;
            if (restored)
            {
                recoverySession.Dispose();
                session = null;
                ShowStatus("복구 완료");
                SetButtonState(ButtonState.Idle);
                SetControlsEnabled(true);
                if (closeWhenStopped) CloseAfterRecovery();
            }
            else
            {
                ShowStatus("복구 확인 필요", restoreError is null
                    ? "복구 검증에 실패했습니다. 다시 눌러 재시도하세요."
                    : "복구 처리 중 오류가 발생했습니다: " + restoreError);
                SetButtonState(ButtonState.Error, recoveryRequired: true);
                SetControlsEnabled(true, recoveryOnly: true);
            }
            return;
        }

        busy = true;
        SetControlsEnabled(false);
        SetButtonState(ButtonState.Running);
        ShowStatus("대상 창 확인 중");

        if (!await RefreshGamesAsync(duringStart: true))
        {
            busy = false;
            if (closeWhenStopped)
            {
                CloseAfterRecovery();
                return;
            }
            SetControlsEnabled(true);
            return;
        }
        if (closeWhenStopped)
        {
            busy = false;
            CloseAfterRecovery();
            return;
        }
        if (GameBox.SelectedItem is not GameTarget target)
        {
            ShowStatus(GameBox.Items.Count == 0 ? "보이는 창 없음" : "대상 창 선택");
            SetButtonState(ButtonState.Idle);
            busy = false;
            SetControlsEnabled(true);
            return;
        }

        NativeEngineSession? activeSession;
        NativeEngineStatus startupStatus;
        try
        {
            activeSession = NativeEngineSession.Start(target, out startupStatus);
        }
        catch (Exception ex)
        {
            busy = false;
            SetControlsEnabled(true);
            ShowStatus("최적화 시작 오류", ex.Message);
            SetButtonState(ButtonState.Error);
            return;
        }

        if (activeSession is null)
        {
            busy = false;
            SetControlsEnabled(true);
            ShowStatus("최적화 시작 오류", startupStatus.Message);
            SetButtonState(ButtonState.Error);
            return;
        }

        session = activeSession;
        closeWhenStopped = false;
        stopRequested = false;
        SetControlsEnabled(false);
        RunButton.IsEnabled = true;
        SetButtonState(ButtonState.Running);
        ShowStatus("CPU 스레드 탐색 중");
        CandidateText.Text = "탐색 중";

        var progress = new Progress<SessionStatus>(UpdateStatus);
        var runTask = activeSession.WaitForCompletionAsync(progress);

        SessionResult result;
        try
        {
            result = await runTask;
        }
        catch (Exception ex)
        {
            activeSession.RequestStop();
            result = new(false, true, "엔진 상태를 확인하지 못했습니다. 복구 확인이 필요합니다: " + ex.Message,
                false, 0, 0);
        }
        finally
        {
            busy = false;
            stopRequested = false;
        }

        CandidateText.Text = result.HasCandidate
            ? $"PID {result.ProcessId} · TID {result.ThreadId}"
            : "없음";

        if (result.RecoveryRequired || activeSession.HasPendingRecovery)
        {
            session = activeSession;
            ShowStatus("복구 확인 필요", result.Message);
            SetButtonState(ButtonState.Error, recoveryRequired: true);
            SetControlsEnabled(true, recoveryOnly: true);
            if (closeWhenStopped) ShowStatus("복구 확인 필요", result.Message + " 창을 닫기 전에 복구를 확인해야 합니다.");
            return;
        }

        activeSession.Dispose();
        session = null;
        SetControlsEnabled(true);
        if (result.Passed)
        {
            ShowStatus("복구 완료");
            SetButtonState(ButtonState.Idle);
        }
        else
        {
            ShowStatus("적용 오류 · 원복 완료", result.Message);
            SetButtonState(ButtonState.Error);
        }
        if (closeWhenStopped) CloseAfterRecovery();
    }

    private void UpdateStatus(SessionStatus status) // 네이티브 후보·상태 표시 갱신
    {
        if (!busy || stopRequested) return;

        if (status.HasCandidate)
        {
            CandidateText.Text = $"PID {status.ProcessId} · TID {status.ThreadId}" +
                (status.Confirmed ? string.Empty : " · 확인 중");
        }
        else
        {
            CandidateText.Text = "탐색 중";
        }

        var hasRestrictedProcesses = status.Message.Contains(
            "제한으로 건너뛴 프로세스", StringComparison.Ordinal);

        if (status.Confirmed)
        {
            SetButtonState(ButtonState.Applied);
            ShowStatus("CPU 설정 적용 확인", "설정 적용은 확인했지만, 게임 성능 변화는 측정하지 않았습니다.");
        }
        else
        {
            SetButtonState(ButtonState.Running);
            ShowStatus(status.Applied ? "CPU 범위 적용 · 후보 탐색 중" : "CPU 후보 분석 중",
                hasRestrictedProcesses ? null : status.Message);
        }
    }

    private void SetButtonState(ButtonState state, bool recoveryRequired = false) // 단계별 버튼 문구·안내 설정
    {
        RunButton.Tag = state.ToString();
        RunButton.ToolTip = state switch
        {
            ButtonState.Idle => "CPU 최적화를 시작합니다.",
            ButtonState.Running => "누르면 실행을 중단하고 원래 설정을 복구합니다.",
            ButtonState.Applied => "누르면 실행을 종료하고 원래 설정을 복구합니다.",
            ButtonState.Error when recoveryRequired => "원래 설정 복구를 다시 확인합니다.",
            ButtonState.Error => "오류 상태를 닫고 시작 상태로 돌아갑니다.",
            ButtonState.Recovering => "중단 및 복구를 처리하고 있습니다.",
            _ => null
        };

        RunButton.Content = state switch
        {
            ButtonState.Idle => "시작",
            ButtonState.Running => "중단",
            ButtonState.Applied => "종료",
            ButtonState.Error => "중단",
            ButtonState.Recovering => "중단",
            _ => string.Empty
        };
    }

    private void ShowStatus(string text, string? detail = null) // 상태·상세 안내 표시
    {
        StatusText.Text = text;
        StatusText.ToolTip = detail;
    }

    private void SetControlsEnabled(bool enabled, bool recoveryOnly = false) // 진행·복구 상태별 입력 제어
    {
        GameBox.IsEnabled = enabled && !recoveryOnly;
        RunButton.IsEnabled = enabled;
    }

    private void Window_Closing(object? sender, CancelEventArgs e) // 세션 종료·원복 전 창 닫기 보류
    {
        if (allowClose) return;
        if (busy)
        {
            e.Cancel = true;
            closeWhenStopped = true;
            stopRequested = true;
            session?.RequestStop();
            SetButtonState(ButtonState.Recovering);
            RunButton.IsEnabled = false;
            ShowStatus("중단 및 복구 중");
            return;
        }

        if (session?.HasPendingRecovery == true)
        {
            e.Cancel = true;
            closeWhenStopped = true;
            ShowStatus("복구 확인 필요");
            SetButtonState(ButtonState.Error, recoveryRequired: true);
            SetControlsEnabled(true, recoveryOnly: true);
            return;
        }

        session?.Dispose();
        allowClose = true;
    }

    private void CloseAfterRecovery() // 복구 완료 후 창 종료 허용
    {
        allowClose = true;
        Close();
    }

    private enum ButtonState
    {
        Idle,
        Running,
        Applied,
        Error,
        Recovering
    }
}
