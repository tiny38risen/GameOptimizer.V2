# GameOptimizer.V2

Windows WPF utility for applying a built-in CPU policy to a selected application. The current behavior, implementation structure, and recovery limits are described in [CODE_GUIDE_KO.md](CODE_GUIDE_KO.md).

Build the solution with Visual Studio MSBuild from a Visual Studio Developer PowerShell that has the C++ desktop workload and .NET desktop tools:

```powershell
msbuild .\GameOptimizer.V2.sln -m -p:Configuration=Release -p:Platform="Any CPU"
```
