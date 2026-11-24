# Helldrop: Demon Ambush — iPhone playtest record

## 2026-11-23 · iPhone 14

All seven training lessons and all nine Episode 1 maps were cleared in the installed native game on a physical iPhone 14 running iOS 27.2.1. Training and interruption testing started on commit `f88df23`. The final campaign pass exercised the gameplay shipped in `9ca8f86`. The later App Store archive, version 1.0 (1) at `40f37e6`, passed Apple’s binary validation. A fresh install of the store-distributed build remains a separate check.

### Training and interruptions

- Cleared First Drop, In the Shadows, Barrel Ambush, Chain Reaction, Hold the Door, Crossfire, and The Gauntlet. The Gauntlet’s final action detonated two barrels. The training completion button opened Episode 1.
- Pressed Home during gameplay, reopened the app, and verified that Resume returned to the same lesson.
- Opened the iOS Game Overlay and saw **Game Mode On**. After returning to the game, tapped the green ring in First Drop, saw **Good job! Lesson cleared.**, and tapped **Next lesson**. In the Shadows opened. This is the run for which the OS setting was explicitly observed.

### Campaign

On the original build, the E1M1 marine escaped at full health while the opening controls were being read. After the fix, the marine waited for the opening move. Both a manual floor tap and the Ambush button summoned a demon. The manual drop receipt showed 21 souls spent. E1M1 through E1M3 were cleared twice across the two iterations. E1M4 through E1M9 were then tested on the revised build.

| Map | Final in-game clear time | Result on the phone |
| --- | ---: | --- |
| E1M1 | 40.9 s | Play E1M2 opened E1M2 |
| E1M2 | 40.8 s | Play E1M3 opened E1M3 |
| E1M3 | 28.8 s | Ordinary clear opened E1M4 |
| E1M4 | 17.4 s | Play E1M5 opened E1M5 |
| E1M5 | 18.2 s | Play E1M6 opened E1M6 |
| E1M6 | 47.0 s | Play E1M7 opened E1M7 |
| E1M7 | 28.9 s | Play E1M8 opened E1M8 |
| E1M8 | 44.3 s | Episode cleared! |
| E1M9 | 23.3 s | Direct level start, victory screen offered Play E1M4 |

**Secret entrance.** A separate QA build on the same phone replayed paid summons in E1M3 at accelerated simulation speed. The marine died in secret sector 74 at 90.7 game seconds. The victory screen offered **Play E1M9**. Tapping it opened E1M9 with 30 souls and the marine at 100% health. This verified the branch on hardware. Discovering it unaided with ordinary touch input remains untested.

### Regression checks and next run

The simulator UI suite passed both tests in [TrainingTests.swift](Tests/TrainingTests.swift). It covers training progression, Home and Resume, portrait and landscape placement, campaign pricing, and battlefield height after a summon. Larger accessibility text was exercised on the simulator. Desktop gameplay and browser regressions also passed.

For the next physical pass:

1. Record the device, OS, build, and starting level. On this Mac, the shared Claude/Codex skill `physical-iphone-testing` documents screenshot capture, touch coordinates, and recovery after input timeouts.
2. On the release build, check a fresh start, the first manual drop, an unaffordable Ambush, and a victory-to-next-map transition. Save the victory frame before advancing and confirm the destination title after the tap.
3. Prioritize the remaining hands-on checks: landscape and larger text on hardware, sound and touch feel, and sustained play for frame pacing or thermal slowdown. The reported clear times are gameplay outcomes, not performance measurements.
