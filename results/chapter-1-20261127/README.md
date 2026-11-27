# DanziLoader-Cpp Chapter 1 playtest

On 27 November 2026 (Tehran local time) a player using an Xbox controller progressed from the GTA VI Chapter 1 boat to the camp on foot and then to flight. Windows, RTX 5080, Series S profile, 50% render scale, relay 2 and a warm cache. The same process remained active for the full 1,800-second observation and was still running at the end. The timer started when the player confirmed boat control. The wall-clock interval includes scene transitions.

| Time after boat control | Game-window capture |
| --- | --- |
| 00:20 | [Boat and package objective](boat.jpg) |
| 10:21 | [On foot, searching the camp](camp.jpg) |
| 27:22 | [Aircraft over water](flight.jpg) |

Across the full 30 minutes, 10,884 visible window changes gave 6.05 FPS, with a p95 visible interval of 240 ms. A separate renderer frame counter advanced by 10,795 over 1,783.7 seconds (6.05 FPS). [Minute-by-minute figures](minutes.csv), [visible-frame metrics](metrics.json), [renderer cross-check](render-counter.json).

A subsequent build recorded **17.5 FPS on the window counter in a cutscene** ([capture](cutscene.jpg)). In separate 60-second gameplay samples from the opening boat scene, visible FPS was 6.95 with joined copies off and 8.30 with them on; the corresponding renderer rates were 7.00 and 8.28 FPS. [Boat capture](boat-followup.jpg), [comparison](boat-comparison.csv).

These are maintainer measurements from one machine. They document player control, progression and a 30-minute run. They do not demonstrate sustained 20 FPS. Water and lighting faults and uneven frame pacing remain visible.
