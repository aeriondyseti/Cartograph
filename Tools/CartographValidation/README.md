# Cartograph validation tools

Scripts used to compare Cartograph builds in a running game: a client for the
`CartographTestBridge` development mod, scenario generation, frame-time and memory
analysis, outside monitors, and spoken cues for attended tests.

Development tooling only. Nothing here is part of the mod, and nothing here is
packaged with it. Python scripts use the standard library only; PowerShell scripts
are Windows-only.

## Requirements

- The game with Cartograph and `Mods/CartographTestBridge` installed, started with
  `-CartographBridge -CartographBridgeDir=<this directory>/Bridge`. Without that
  launch option the bridge does nothing.
- Python 3.9 or later.
- PowerShell 7 for the monitors; Windows PowerShell 5.1 must also be present, the
  audio worker runs in it for the speech engine.
- `nvidia-smi` on the path for `monitor_gpu.ps1`.

## Runtime root

Every script treats **its own directory** as the root and reads and writes beside
itself. None takes a machine path from its source. Generated output is ignored by
the `.gitignore` here:

| Path | Written by | Contents |
|---|---|---|
| `Bridge/` | the game and `bridge_client.py` | `commands.jsonl`, `results.jsonl`, `bridge_status.json`, `samples/*.csv`, `dumps/` |
| `Runs/<run id>/` | `bridge_client.py run`, the analysers | the scenario as run, results, summaries |
| `phase.txt` | `bridge_client.py run` | the command in progress, read by the monitors to label samples |
| `AudioCues/` | `audio_cue.ps1` | rendered WAVs, `manifest.json`, `operations.jsonl`, per-call logs |
| `*.csv` | the monitors | one row per second |

To keep evidence of a session, copy these out; do not commit them. Scenario files
made for a particular save hold that save's name and coordinates, so they are
local too.

## Files

| File | Purpose |
|---|---|
| `bridge_client.py` | `send` one command, or `run` a scenario file of up to 500 commands with assertions (`_expect`, `_expect_near`, `_expect_min`, `_expect_delta`, `_capture`, `_same_hash_as`, `_different_hash_as`, `_same_data_as`). A request id is written once; rerunning with the same run id resumes without repeating completed commands. |
| `make_onscreen_scenarios.py` | Writes `onscreen_preflight.json`, `onscreen_preflight_cleanup.json` and `onscreen_measure.json` from a saved `view` result, a save name and optionally an explicit layout. Submits nothing. |
| `analyze_frames.py` | Per sample: p50, p95, p99, maximum, counts over 33.3, 50 and 100 ms, full-redraw intervals, and the three worst frames with the commands that overlap them. Overlap is correlation, not cause. |
| `summarize_run.py` | Command durations and, with `--memory`, memory per phase from `monitor_memory.ps1` output. |
| `analyze_manual.py` | Frame statistics for the action windows of a guided manual pass, defined by the audio log. |
| `monitor_memory.ps1` | Process private bytes, working set and per-process GPU memory counters, once per second. |
| `monitor_gpu.ps1` | Whole-adapter utilisation and memory from `nvidia-smi`. Includes every other process using the adapter. |
| `monitor_focus.ps1` | Whether the game is the foreground window. Reads the window's process id only: no titles, input or screenshots. |
| `audio_cue.ps1`, `audio_cue_worker.ps1` | Render and play spoken cues. See `AUDIO_CUES.md`. |
| `manual_audio_sequence.ps1` | Plays the step instructions of a manual pass in order, with pauses. |

Each monitor refuses to overwrite an existing output file and stops at its
duration or when its stop file appears (`stop-monitor`, `stop-gpu-monitor`,
`stop-focus-monitor`).

## Typical use

```powershell
# outside monitors, each in its own terminal
./monitor_memory.ps1 -OutputName memory-01.csv
./monitor_gpu.ps1 -OutputName gpu-01.csv
./monitor_focus.ps1 -OutputName focus-01.csv

# one command
python bridge_client.py send state

# a scenario
python bridge_client.py run path/to/scenario.json --run-id candidate_01
python analyze_frames.py candidate_01
python summarize_run.py candidate_01 --memory memory-01.csv
```

## Things that went wrong in practice

- **Compare builds under one display state.** The bridge's `view` reports the
  same viewport size and window mode whether the game is truly fullscreen or
  shown as a borderless image, so it cannot gate on this. The person at the screen
  has to confirm it. A window rectangle read from outside can serve as a warning
  check once calibrated against such a confirmation, for that session only.
- **A focus change is expensive.** Losing and regaining fullscreen rebuilt the
  swap chain and cost about 1.7 s each way on the test machine. Global shortcuts
  of other applications can cause it. Keep such frames in the data, labelled, with
  `monitor_focus.ps1` output and the game log as evidence.
- **Sample times are engine frames.** With frame generation enabled they say
  nothing about presented frames.
- **A load does not restore the camera pitch.** Follow `load_save` with
  `set_view` before judging what is on screen.
- **`snap_to_ground` takes the first hit of a trace straight down** from 50 m
  above each slot. Over a gap it finds the terrain, under a roof it finds the
  roof. On a platform, give an explicit height and turn it off.
- **`verify_indices` checks Cartograph's own structures only**, not the world.
  To check that hand-made changes were tracked, compare `building_count` before a
  save with the count after loading it.
- **`analyze_manual.py` maps sample time to wall-clock time through
  `bridge_status.json`**, which belongs to the running game process. Run it before
  the game is restarted, or the mapping is wrong.
- **The mod's config file carries a version stamp.** Installing a build of another
  version rewrites it, so the file's hash changes though no setting did. Compare
  the settings each build logs at load, not the hash.
- **`Recipe_SmelterMk1` builds a foundry**, going by the class the bridge reports
  (`Build_FoundryMk1_C`). The scenario generator's `foundry` entry is right.
- Do not build, cook or package on the machine while a sample is running.

## Privacy

Scenario files, saves, inventories, logs, dumps and recordings from a test session
describe a particular machine and save. They stay out of the repository.
