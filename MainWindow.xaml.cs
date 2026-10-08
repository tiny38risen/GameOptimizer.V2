using System.ComponentModel;
using System.Windows;

namespace GameOptimizer.V2;

public partial class MainWindow : Window
{
    private GameSession? session;
    private CancellationTokenSource? cancellation;
    private Task<SessionResult>? runTask;
    private bool closeWhenStopped;
    private bool allowClose;
    private bool busy;

    public MainWindow()
    {
        InitializeComponent();
        Loaded += async (_, _) => await RefreshGamesAsync();
    }

    private async Task RefreshGamesAsync()
    {
        if (busy) return;
        var selected = GameBox.SelectedItem as GameTarget;
        try
        {
            var games = await Task.Run(GameTarget.FindVisibleGames);
            GameBox.ItemsSource = games;
            GameBox.SelectedItem = games.FirstOrDefault(game => selected is { } previous &&
                game.ProcessId == previous.ProcessId && game.StartTimeUtcTicks == previous.StartTimeUtcTicks)
                ?? games.FirstOrDefault();
            StatusText.Text = games.Count == 0 ? "선택할 수 있는 게임 창이 없습니다." : "게임을 선택하세요.";
        }
        catch (Exception ex)
        {
            StatusText.Text = "게임 목록을 읽지 못했습니다: " + ex.Message;
        }
    }

    private async void GameBox_DropDownOpened(object? sender, EventArgs e) => await RefreshGamesAsync();

    private async void Run_Click(object sender, RoutedEventArgs e)
    {
        if (busy)
        {
            StatusText.Text = "중단 및 복구 중입니다…";
            cancellation?.Cancel();
            RunButton.IsEnabled = false;
            return;
        }

        if (session?.HasPendingRecovery == true)
        {
            busy = true;
            SetControlsEnabled(false);
            StatusText.Text = "복구를 다시 확인합니다…";
            bool restored = await Task.Run(session.TryRestore);
            busy = false;
            if (restored)
            {
                session.Dispose();
                session = null;
                StatusText.Text = "원래 설정으로 복구했습니다.";
                if (closeWhenStopped) CloseAfterRecovery();
            }
            else
            {
                StatusText.Text = "복구를 확인하지 못했습니다. 새 적용은 차단되어 있습니다. 다시 눌러 복구를 재시도하세요.";
                RunButton.Content = "복구 재시도";
                SetControlsEnabled(true, recoveryOnly: true);
            }
            return;
        }

        if (GameBox.SelectedItem is not GameTarget target) return;
        session = new GameSession();
        cancellation = new CancellationTokenSource();
        busy = true;
        closeWhenStopped = false;
        SetControlsEnabled(false);
        RunButton.IsEnabled = true;
        RunButton.Content = "중단 및 복구";
        StatusText.Text = "대상 확인 중…";
        CandidateText.Text = "후보를 관찰하는 중…";

        var progress = new Progress<SessionStatus>(UpdateStatus);
        var activeSession = session;
        var token = cancellation.Token;
        runTask = Task.Run(() => activeSession.RunAsync(
            target, CpuPlacement.TwoLogicalProcessors, raisePriority: true,
            progress: progress, cancellationToken: token));

        SessionResult result;
        try
        {
            result = await runTask;
        }
        catch (Exception ex)
        {
            result = new(false, false, "실행 오류: " + ex.Message, null);
        }
        finally
        {
            busy = false;
            cancellation?.Dispose();
            cancellation = null;
            runTask = null;
        }

        StatusText.Text = result.Message;
        CandidateText.Text = result.Candidate is { } candidate
            ? $"PID {candidate.ProcessId} · TID {candidate.ThreadId} · CPU 후보"
            : "없음";

        if (result.RecoveryRequired || activeSession.HasPendingRecovery)
        {
            session = activeSession;
            RunButton.Content = "복구 재시도";
            SetControlsEnabled(true, recoveryOnly: true);
            if (closeWhenStopped) StatusText.Text += " 창을 닫기 전에 복구를 확인해야 합니다.";
            return;
        }

        activeSession.Dispose();
        session = null;
        RunButton.Content = "추적 시작";
        SetControlsEnabled(true);
        if (closeWhenStopped) CloseAfterRecovery();
    }

    private void UpdateStatus(SessionStatus status)
    {
        StatusText.Text = status.Message;
        CandidateText.Text = status.Candidate is { } candidate
            ? $"PID {candidate.ProcessId} · TID {candidate.ThreadId} · " +
              (status.Confirmed ? "안정 후보" : "안정성 확인 중")
            : "확인 가능한 후보 없음";
    }

    private void SetControlsEnabled(bool enabled, bool recoveryOnly = false)
    {
        GameBox.IsEnabled = enabled && !recoveryOnly;
        RunButton.IsEnabled = enabled;
    }

    private void Window_Closing(object? sender, CancelEventArgs e)
    {
        if (allowClose) return;
        if (busy)
        {
            e.Cancel = true;
            closeWhenStopped = true;
            cancellation?.Cancel();
            RunButton.IsEnabled = false;
            StatusText.Text = "종료 및 복구가 끝날 때까지 기다립니다…";
            return;
        }

        if (session?.HasPendingRecovery == true)
        {
            e.Cancel = true;
            closeWhenStopped = true;
            StatusText.Text = "복구 미확인 상태라 창을 닫을 수 없습니다. 복구를 재시도하세요.";
            RunButton.Content = "복구 재시도";
            SetControlsEnabled(true, recoveryOnly: true);
            return;
        }

        session?.Dispose();
        allowClose = true;
    }

    private void CloseAfterRecovery()
    {
        allowClose = true;
        Close();
    }
}
