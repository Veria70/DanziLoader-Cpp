# Windows in-game session

A local C++ build ran on a Windows x64 PC with an NVIDIA GeForce RTX 5080. The run used the Series S profile, 50% render scale, Direct3D 12 and relay level 2. DRED command history was enabled and the Direct3D debug layer was off. The session reused a warm cache. All timestamps below are UTC on 26 November 2026.

| Observation | Evidence |
| --- | --- |
| Progress | The prison interior rendered with the interaction prompt. The player opened the in-game menu and reported reaching Chapter 1. A later window capture showed the controllable boat with the objective to catch the plane. |
| Session duration | More than 16 minutes elapsed between the first captured prison scene at 20:44:02 and the boat scene at 21:00:16 in the same process. This includes transitions and menus. The operator ended the session. An earlier run lost the D3D12 device near the prison. |
| Longer frame-counter interval | Presented frames 2048 and 8192 were logged at 20:45:14.727265 and 20:56:57.325807. The 6144-frame advance over 702.599 seconds gives 8.74 FPS across this mixed interval. The log does not identify time spent in menus. |
| Chapter 1 sample | The player identified the chapter. The runtime frame counter advanced from 8192 at 20:56:57.325807 to 8693 at 20:57:42.806618. The 501-frame advance over 45.481 seconds gives an estimated 11.02 FPS. |
| Water and lighting | Characters, textured rooms, lighting and reflections were visible. A patch resembling the riverbed appeared in the water beside the boat. The window reported 7.5 to 30 FPS across the visible boat samples. These individual readings fluctuate sharply. |

The session used a local working build. The publication checkout was separately rebuilt in Release mode and passed all 81 core unit tests. The session has no independent reproduction or verified full-session gameplay-only average.
