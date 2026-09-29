"""Analyze guided manual action windows using audio UTC and live bridge clock mapping.

Run before the game restarts: bridge_status must belong to the same process as
the requested sample. Frames overlapping speech or action-window boundaries are
excluded. Foreground samples remain separate evidence, at one-second cadence.
"""
import argparse
import csv
import datetime as dt
import json
import math
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument('sample')
p.add_argument('--focus', type=Path, required=True)
a = p.parse_args()
root = Path(__file__).parent
utc = lambda s: dt.datetime.fromisoformat(s.replace('Z', '+00:00')).timestamp()
status = json.loads((root/'Bridge/bridge_status.json').read_text(encoding='utf-8-sig'))
offset = utc(status['wall_clock_utc']) - status['t']
rows = list(csv.DictReader((root/'Bridge/samples'/f'{a.sample}.csv').open(encoding='utf-8-sig')))
for r in rows:
    r['end_utc'] = float(r['t']) + offset
    r['start_utc'] = r['end_utc'] - float(r['delta_ms']) / 1000
ops = [json.loads(line) for line in (root/'AudioCues/operations.jsonl').read_text(encoding='utf-8-sig').splitlines() if line.strip()]
ops = [o for o in ops if o['action'] == 'Play' and o['exit_code'] == 0
       and rows[0]['start_utc'] <= utc(o['started_utc']) <= rows[-1]['end_utc']]
focus = list(csv.DictReader(a.focus.open(encoding='utf-8-sig')))
report = {'sample': a.sample, 'clock_mapping_status': status, 'utc_offset_seconds': offset,
          'note': 'Action windows exclude audio and boundary-crossing frames; process clock mapping requires no restart.',
          'windows': [], 'raw_worst_frames': []}
for cue, following in [('ManualFoundries','ManualAssemblers'), ('ManualAssemblers','ManualDismantle'), ('ManualDismantle','ManualHold')]:
    starts = [o for o in ops if o['cue'] == cue]
    ends = [o for o in ops if o['cue'] == following]
    if len(starts) != 1 or len(ends) != 1:
        raise ValueError(f'Expected exactly one {cue}/{following} cue within sample')
    begin, end = utc(starts[0]['finished_utc']), utc(ends[0]['started_utc'])
    selected = [r for r in rows if r['start_utc'] >= begin and r['end_utc'] <= end]
    values = sorted(float(r['delta_ms']) for r in selected)
    fr = [r for r in focus if begin <= utc(r['utc']) <= end]
    entry = {'cue': cue, 'start_utc': starts[0]['finished_utc'], 'end_utc': ends[0]['started_utc'],
             'frames': len(values), 'p50_ms': values[math.ceil(len(values)*.5)-1],
             'p95_ms': values[math.ceil(len(values)*.95)-1], 'p99_ms': values[math.ceil(len(values)*.99)-1],
             'max_ms': max(values), 'over_50_ms': sum(v>50 for v in values),
             'over_100_ms': sum(v>100 for v in values), 'focus_readings': len(fr),
             'focus_not_true': sum(r['game_is_foreground'] != 'True' for r in fr)}
    report['windows'].append(entry)
for r in sorted(rows, key=lambda r: float(r['delta_ms']), reverse=True)[:5]:
    report['raw_worst_frames'].append({'frame':r['frame'], 'delta_ms':float(r['delta_ms']),
        'end_utc':dt.datetime.fromtimestamp(r['end_utc'], dt.timezone.utc).isoformat(),
        'overlapping_audio':[o['cue'] for o in ops if utc(o['started_utc']) <= r['end_utc'] and utc(o['finished_utc']) >= r['start_utc']]})
out = root/'Runs'/a.sample
out.mkdir(parents=True, exist_ok=True)
(out/'manual_analysis.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
print(json.dumps(report, indent=2))
