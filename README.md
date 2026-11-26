# DanziLoader-Cpp

DanziLoader-Cpp is a C++20 port of the DanziLoader / VIctory Windows x64 compatibility runtime. It includes the title loader, runtime services, shader translator, Direct3D 12 renderer and bundled libopus decoder.

## In-game status

A Windows playtest on an RTX 5080 reached the prison, Chapter 1 and the boat pursuit using the Series S profile at 50% render scale. The same process stayed up for more than 16 minutes from the prison to the boat. A 45.481-second Chapter 1 sample averaged approximately 11.02 FPS. Water rendering and frame pacing still show visible faults. An earlier run ended after a GPU timeout.

See the [session measurements](https://github.com/Veria70/DanziLoader-Cpp/blob/d516f9ea52dfbddff20e9acbdb23e24801cc6442/results/windows-ingame/README.md) for the longer frame-counter interval, timing method, settings and limitations.

## Build the runtime

From an x64 Visual Studio developer shell with MSVC, the Windows SDK, CMake and Ninja installed, run

```powershell
cmake --preset msvc-release
cmake --build --preset msvc-release --target victory danzi_backend
```

Keep `danzi_launcher.dll` beside `victory.exe`. See [NOTICE.md](NOTICE.md) for the upstream projects and [LICENSE](LICENSE) for the licence.
