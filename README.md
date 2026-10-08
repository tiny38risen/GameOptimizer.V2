# GameOptimizer.V2

A small Windows app with a C++ native engine and a WPF front end. The native engine tracks CPU-active game threads, applies a fixed scheduling policy to a stable candidate, and verifies restoration.

- Select a running game with a visible window.
- Press the single action button to track candidates and apply affinity to two allowed logical processors plus `AboveNormal` priority.
- Press it again, or close the app normally, to restore the original settings.
- If restoration cannot be verified, the app blocks new changes and keeps the recovery action available.

The candidate is inferred from thread CPU-time deltas; it is not confirmed to be the game's input or main thread. WPF handles game selection and display. `GameOptimizer.Engine.dll` owns thread observation, candidate confirmation, affinity/priority changes, and recovery. The app targets .NET 8 and x64, and may need to run as administrator when the game is elevated.

Build the solution with Visual Studio MSBuild from a Visual Studio Developer PowerShell that has the C++ desktop workload and .NET desktop tools:

```powershell
msbuild .\GameOptimizer.V2.sln -m -p:Configuration=Release -p:Platform="Any CPU"
```
