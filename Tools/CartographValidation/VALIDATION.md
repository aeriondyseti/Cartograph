# Cartograph fix and validation

2026-09-29. Production candidate: **`b2e9030d63` (candidate7)** on
`update-cl502094`. Public copy of the validation report; names of people, of
other applications, save identifiers, coordinates and machine paths are left out.

## Recommendation

Use the combined candidate: the VRAM improvements, PR19, selected PR20 changes,
and the lifecycle/correctness fixes found during this investigation. **PR19 alone
does not fix redraw restart starvation.** Keep PR20's disk-image cache excluded:
its synchronous readback adds save-time work and its cache validity test does not
cover spline geometry.

Selected PR20 commits: `255ccb9922`, `b4f0bac5f7`, `bb23446144`.
Excluded cache commit: `47686c7d5a`.

## Causes established by code review and tests

1. **Construction restarted an unfinished full redraw.** Continuous changes kept
   cancelling and restarting it. Letting a full redraw finish, then applying
   queued changes, removes this starvation. The baseline redraw remained active
   through each 15-second placement batch; candidate finished while placement
   continued.
2. **Normal map-key close bypassed the old close hook.** The map disappeared but
   Cartograph still considered it visible, retained the render target, and kept
   drawing. Native widget lifecycle notifications now update visibility, cancel
   obsolete free-on-close work, and release the target. Rapid reopening requests
   a complete replacement when a cancelled partial result is stale.
3. **A shared world canvas was retained across coroutine yields.** Another canvas
   user could overwrite it; a controlled probe demonstrated interference.
   Candidate owns its canvas and passes the coexistence/teardown checks.
4. **Synchronous coroutine completion could lose the active handle.** An initial
   gather could finish inside its launching call and start a redraw; the outer
   assignment then replaced that active handle with the completed gather.
   Resident-mode testing reproduced overlapping-draw assertions. Candidate7's
   generation guard prevents the stale assignment; same-frame gathers and
   resident/early-open regressions passed afterward.
5. **Index and clipping defects risked incorrect partial updates.** Invisible
   entries shifted the building array without shifting its redirectors. Index
   updates now account for all entries, and invalid/nonfinite/out-of-range
   scissor bounds are rejected or clamped. Native scissor support replaces the
   custom global canvas hook.

Initialization's unnecessary quadratic index work was also removed. The early
hypothesis of one deferred whole-map EndDraw burst was disproven: this engine's
canvas flushes these DrawItem calls immediately.

These findings explain demonstrated redraw/lifecycle failures. They do **not**
prove the cause of every original freezing or driver-timeout report.

## Foreground fullscreen construction comparison

Game 1.2.4.0 / CL 502094, RTX 5060 Ti, 15,195 tracked buildings, 8192 map texture,
mips enabled, free-on-close enabled, budgets 30 ms initialization / 1 ms redraw.
Same native-loaded save, camera, elevated foundation platform, 16 machines at
one per second, followed by removal. The tester confirmed all grid corners
visible and confirmed both candidate automated runs used true fullscreen.
Baseline was launched and visually confirmed the same way. No build/cook
overlapped samples.

Display: 2560x1440, screen percentage 75, VSync requested, cap 60, frame generation
enabled per user settings. These are **engine-frame times**, not displayed FPS.
The tester observed tearing; requested VSync does not establish synchronized
presents.

### Real map UI: open, close during redraw, then build in view

| Phase | Baseline p95 / p99 | Candidate p95 / p99 | Frames >50 ms: baseline / candidate |
|---|---:|---:|---:|
| Foundries |48.19 / 52.69 ms|36.50 / 41.14 ms|30 / 2|
| Assemblers |49.54 / 54.12 ms|35.69 / 39.76 ms|55 / 2|

Baseline continued full drawing after the map closed for total active intervals
of 18.59/18.53 seconds. Candidate stopped obsolete work after 0.68/0.75 seconds;
those are cancellation intervals, not completed redraw durations. Both builds
had zero frames over 100 ms. Idle p99 was 36.1–36.6 ms.

### Synthetic stress: map drawing active with the world visible

This uses the direct test entry point without displaying the map overlay.

| Phase | Baseline full redraw | Candidate full redraw | Baseline / candidate p95 | Frames >50 ms: baseline / candidate |
|---|---:|---:|---:|---:|
| Foundries |18.331 s|2.456 s|49.63 / 38.68 ms|54 / 12|
| Assemblers |18.326 s|2.582 s|48.72 / 36.47 ms|35 / 5|

Completed redraws finished about 7.1–7.5 times sooner under this workload. Both
builds again had zero frames over 100 ms. Counts returned to the starting values
and internal index checks passed in all four automated scenarios. All recorded
focus samples during their measured phases showed the game foreground.

These are one execution per build per scenario, consistent with earlier clean
off-screen A/B runs, not a population estimate. The visible-factory run used
29–57% global GPU utilization during candidate real-UI samples; it did not
saturate the GPU. Global VRAM was 12,952–13,302 MiB, including roughly 8 GB held by
a separate process left unchanged during testing.

### Remaining map-open cost

Real map opening took 77.1/75.1 ms frames on baseline and 83.1/81.9 ms on candidate.
Direct opens were 29.5–32.9 ms across both builds. The roughly 6 ms direction is
recorded, but these single passes cannot isolate its cause. The real UI includes
both the game's and Cartograph's widgets. This is a follow-up, not evidence of
a construction freeze. No extra resident-mode timing experiment was added.

## Memory, save, and functional checks

- Five control-save load/draw/save/menu cycles: candidate menu dedicated GPU
  memory was about 351–355 MB lower than the PR17-based reference, consistent with
  releasing the 8K mipmapped target. Small per-cycle growth was similar on both.
- A native-loaded large migrated save had 46,259 tracked entries. Candidate6
  completed five native load/draw/save/menu cycles: about 95 MB private-RAM and
  10 MB dedicated-GPU growth across the run, with the target released at every
  menu. Saves took 0.65–0.69 seconds. Candidate7 passed two further native cycles.
- Resident 4K, mips-off behavior passed on candidate7: hidden changes update the
  retained target, opening an up-to-date map avoids a full redraw, and menu
  teardown releases it. The startup race above was exercised and fixed here.
- Incremental versus full pixels, reopen, filters, height ranges, rapid close/open,
  partial-cancel/reopen, canvas coexistence, and exit during redraw passed.
- Forty configured machines retained exact recipes, input inventories, paused
  state and potential settings across native save/reload. They were unpowered;
  this does not validate active production simulation.
- The tester manually placed ten foundations extending into open air, saw them on
  Cartograph, dismantled them, and confirmed they disappeared. Four earlier ones,
  placed over existing foundations, were not used as visual evidence.
- Candidate and baseline attended logs were archived with no Cartograph errors,
  failed ensures, assertions, fatal errors, or new crash folders.

## Final guided manual test

The tester asked for five foundries and five assemblers, with 30 seconds per
step. After a fresh normal launch and visual fullscreen confirmation, native load
and teleport restored the same platform position. Counts progressed
15,195 -> 15,200 -> 15,205 -> 15,195, with no internal index errors afterward.

| Action window | p99 | Maximum | Frames >50 ms |
|---|---:|---:|---:|
| Five foundries |36.407 ms|42.162 ms|0|
| Five assemblers |37.676 ms|46.038 ms|0|
| Dismantle ten |38.080 ms|92.402 ms|4|

Windows are defined from the end of each spoken instruction to the start of the
next. All 89 one-second focus readings within those windows showed the game
foreground. The full raw recording is retained, including two long frames
(1.786 and 1.746 seconds) overlapping the dismantle instruction.

Those long frames coincide with a confirmed external interruption: the foreground
switched to another application at 21:13:14–15 UTC; the tester confirmed
accidentally triggering that application's global hotkey. The game log records
swapchain resizes at 21:13:15.557 and 21:13:17.627, matching the long-frame
endpoints, with frame generation disabled and re-enabled around them. Cartograph
redraw/pending flags and change queues were zero in surrounding sampled rows. The
92.402 ms frame immediately follows the second resize. This interval is labelled
as interrupted presentation and is not silently discarded or claimed as
hitch-free gameplay. No additional run is required to reproduce that known
interruption.

The tester subsequently reported that placing the five foundries and five
assemblers was smooth, without stutters or hitches, and that the instructions
allowed ample time. Screen tearing remained noticeable. The statement is recorded
as a subjective construction result, alongside the frame measurements.

## Important limits

The automated foreground fullscreen comparison placed one machine per second.
The tester reports being able to place two to three per second; the manual run
covers ten machines at the tester's chosen pace on the candidate only, without a
controlled comparison or established sustained rate.
Earlier ten-per-second foundry/assembler runs used off-screen placements.

The reference already includes PR17 and earlier VRAM work. The original upstream
multi-GB leak and reported 15-second whole-game freeze were not independently
reproduced against it. Bounded cycles do not establish indefinite leak freedom.
Earlier bridge versions restored world data through map travel but created a
fresh player; final native-loader tests cover saved-player restoration.

The first guided manual machine timing occurred in pseudo-fullscreen, as reported
by the tester, who later clarified that its first two machines were **smelters,
not foundries**; the spoken cue and raw sample labels describe the requested
task, not the actual machine class. That run is not used as manual foundry
evidence. The final five-each repeat used the correct foundries and assemblers.
The first run's action windows had no frame over 50 ms and counts tracked
+2, +2, -4 correctly, but it is not accepted as normal fullscreen performance.
Why that run stayed in pseudo-fullscreen is not established. Raw setup frames,
including a 1.73-second focus-transition frame before the first cue, are
preserved and excluded only by predefined audio action windows.

A separate frame-generation present crash occurred during display-mode changes.
Its relation to Cartograph is unproven; no connection to the original AMD timeout
is claimed. Save-boundary RGB differences also occur on the reference with
unchanged alpha coverage; overlap ordering is a hypothesis, not an established
cause.

Screen tearing was observed throughout with frame generation enabled. It was not
investigated and its cause is not established.

## Testing side effects

The final save-folder comparison found **three overwritten original autosave
slots**. Test copies retained the original world's session name, so periodic
game autosaves used that session's rotating slots despite all explicit bridge
saves using a test name prefix. Earlier statements that original saves were
untouched were incorrect. Original versions remained intact in the backup taken
before testing. The owner chose to leave the current autosaves as they are.

The autosaves' last-write times fall outside the measured action windows and
automated runs. File timestamps do not establish autosave start or duration, and
frame maxima cannot prove absence of autosave work. One falls within the untimed
foundation/map check. Future tests must use a separate save-session identity or
explicitly disable autosave with a restoration plan.

Before cleanup the audit found 359 files against 336 in the backup: 332 identical,
four changed (the three autosaves and the game's server-list file), none missing,
and 23 new test-save files. No other original save changed.

An earlier test launch with display arguments left a "last confirmed" resolution
of 1600x900 in the display settings file beside the active 2560x1440 setting.
This was disclosed; it has not been shown to cause the presentation issue. The
file was identical across the compared attended sessions.

## Build and evidence provenance

- Candidate Cartograph DLL SHA256:
  `4E96DC73E2A43F555E12FE2E01314B5F9B2DB98BE6C0A7BC493A810E692578AB`
- Reference Cartograph DLL SHA256:
  `941290352296B5616281C542A179763E67DBDEFE657C4BA8D2BE03A941E1274A`
- Matched bridge 0.5.2 pairs were used; the bridge is a separate development mod.
- The baseline rewrote only the config version stamp from 1.3.1 to 1.3.0;
  rendering settings stayed identical. Different whole-file hashes do not mean
  different benchmark settings. Both builds had free-on-close enabled; the
  reference's map-key close notification defect prevented it taking effect.
- The same two scenario files were run verbatim against candidate and baseline.
- Runs: `candidate7_platform_realui_01`, `candidate7_platform_synthetic_01`,
  `baseline4_platform_realui_01`, `baseline4_platform_synthetic_01`.
- Frame samples, command results, audio operation timestamps, focus,
  process-memory and GPU recordings, scenario files and the starting save are
  kept locally and are not published.

After testing, the candidate stayed installed, the original Cartograph
configuration was restored and verified, and the bridge was removed from the
game. The tools used are in this directory; see `README.md`.
