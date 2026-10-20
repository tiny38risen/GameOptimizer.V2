using System.Windows;

namespace GameOptimizer.V2;

public partial class App : Application
{
    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);
        if (e.Args.Length > 0 && e.Args[0] == "--watchdog")
        {
            ShutdownMode = ShutdownMode.OnExplicitShutdown;
            _ = Task.Run(() =>
            {
                RecoveryWatchdog.Run(e.Args);
                Dispatcher.Invoke(Shutdown);
            });
            return;
        }

        RecoveryWatchdog.RestoreStaleJournals();
        new MainWindow().Show();
    }
}
