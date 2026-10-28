# HarmonyRecomp Setup

Source of `HarmonyRecomp-Setup.exe`, the installer attached to every release.

It asks for the player's own game file (and optional DLC folder), then:

1. downloads `harmonyrecomp-package.zip` (prebuilt ReXGlue SDK, this project's
   source, embedded Python) and `harmonyrecomp-toolchain.zip` (Clang/LLD,
   CMake, Ninja) from the same GitHub release (or uses them if they lie next
   to the setup program: offline install),
2. if Visual Studio's C++ tools are not installed, downloads Microsoft's C/C++
   runtime headers and libraries and the Windows SDK from Microsoft after the
   user accepts Microsoft's license (`scripts/portable_msvc.py`),
3. runs `scripts/build.py --game ... --sdk-dir ... --install-to ...`, which
   extracts the game, runs codegen and compiles,
4. creates shortcuts and removes the build files.

No game code or data is ever downloaded or distributed. The release workflow
(`.github/workflows/release.yml`) builds the SDK package, the toolchain and
this program.

Build locally (Windows, .NET SDK 8 or newer):

```
dotnet build installer/HarmonyRecompSetup.csproj -c Release -p:ReleaseTag=v1.0.0
```

The program targets .NET Framework 4.8, which every Windows 10 (1903 or newer)
and Windows 11 already has.
