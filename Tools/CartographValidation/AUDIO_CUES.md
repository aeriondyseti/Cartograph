# Cartograph audio cues

Spoken cues for attended tests, where the game covers the screen and the tester
cannot read a terminal.

Cue preparation renders local WAV files without opening the audio device. Playback
uses Windows' default audio routing and launches with a hidden window to avoid
taking focus. It does not change volume settings, resolution, or game input.
Routed or remote audio may go to another device; the tester must confirm the
audible device and volume.

## Prepare silently

```powershell
./audio_cue.ps1 -Action Render -Volume 40
```

This creates every cue in `AudioCues/`, plus a manifest recording voice, text, size
and synthesis volume. Re-render at a different volume only after the tester's
feedback; this adjusts the WAVs, not Windows volume. Rendering needs the Windows
speech engine, which can fail to initialize inside a restricted sandbox.
`operations.jsonl` records every helper call; a successful playback return does not
establish that anyone heard it.

## Test with the tester present

```powershell
./audio_cue.ps1 -Action Play -Cue Test
```

Confirm the cue reaches the intended device, is audible over the game, and is
comfortable. File validation alone is not proof of audible playback. No test
phrase should play while the tester is away.

## Use during attended tests

- Start: play before sampling begins; helper returns when playback completes.
- Finish: stop sampling first, then play the cue.
- Manual: prepare the tester for the agreed start time; keep cue playback outside
  the measured interval.
- Abort: play after stopping/invalidating a failed or interrupted sample.
- One operator owns playback; whoever runs the scenario sends explicit cue
  requests. Only one party should play a given cue.
- Always agree a finish time beforehand. The tester may check the terminal at that
  time if no cue is heard; mark/discard interrupted samples rather than extending
  an invisible wait.
- Tell the tester explicitly when to bring the game to the foreground, and check
  the foreground window before playing Start.

## Step-by-step manual instructions

`manual_audio_sequence.ps1 -Phase Machines` plays ManualFoundries, ManualAssemblers,
ManualDismantle, ManualHold. The first three have 30-second pauses after playback.
Start sampling before playback and exclude the logged audio operations from the
performance analysis, then stop sampling and capture state after ManualHold.
Only after that run `-Phase MapCheck`: FoundationBuild, FoundationMap,
FoundationRemove, FoundationVerify, ManualReport, with 30-second pauses after each
action. The last cue explicitly releases the tester. No generic Finish cue between
phases. `-DescribeOnly` displays the schedule without playback. Creating
`stop-manual-audio` beside the runner cancels at its next check; remove it
afterwards.

ManualBuild, ManualMap, ManualRemove and ManualDone are an earlier one-cue-per-part
wording. The sequence runner does not use them.
