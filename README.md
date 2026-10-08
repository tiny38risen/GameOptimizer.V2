# GameOptimizer.V2

A small Windows WPF app that tracks CPU-active threads in a selected game and applies a fixed scheduling policy to a stable candidate.

- Select a running game with a visible window.
- Press the single action button to track candidates and apply affinity to two logical processors plus `AboveNormal` priority.
- Press it again, or close the app normally, to restore the original settings.
- If restoration cannot be verified, the app blocks new changes and keeps the recovery action available.

The candidate is inferred from thread CPU-time deltas; it is not confirmed to be the game's input or main thread. The app targets .NET 8 WPF and may need to run as administrator when the game is elevated.

Build with `dotnet build GameOptimizer.V2.csproj -c Release`.
