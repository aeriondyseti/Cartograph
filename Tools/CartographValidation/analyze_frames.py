"""Frame distributions and full-redraw intervals from a completed bridge scenario."""
import argparse
import csv
import json
import math
import pathlib

root = pathlib.Path(__file__).parent
parser = argparse.ArgumentParser()
parser.add_argument('run_id')
args = parser.parse_args()
results = json.loads((root/'Runs'/args.run_id/'results.json').read_text())
report = []
for stop in (r for r in results if r['cmd'] == 'sample_stop'):
    data = stop['data']
    with (root/'Bridge'/data['file']).open(newline='', encoding='utf-8-sig') as source:
        rows = list(csv.DictReader(source))
    values = sorted(float(r['delta_ms']) for r in rows)
    if not values:
        continue
    start_t = stop['t_end'] - data['duration']
    entry = {'name': data['name'], 'frames': len(rows),
             'p50_ms': values[math.ceil(len(values)*0.50)-1],
             'p95_ms': values[math.ceil(len(values)*0.95)-1],
             'p99_ms': values[math.ceil(len(values)*0.99)-1],
             'max_ms': values[-1],
             'frames_over_33_333ms': sum(v > 1000 / 30 for v in values),
             'frames_over_50ms': sum(v > 50 for v in values),
             'frames_over_100ms': sum(v > 100 for v in values),
             'full_redraw_intervals': [], 'actions': [], 'worst_frames': []}
    for row in sorted(rows, key=lambda r: float(r['delta_ms']), reverse=True)[:3]:
        frame_end = float(row['t'])
        frame_start = frame_end - float(row['delta_ms']) / 1000
        overlaps = [r['cmd'] for r in results
                    if r['t_start'] <= frame_end and r['t_end'] >= frame_start]
        entry['worst_frames'].append({'frame': row['frame'], 't': frame_end,
                                     'delta_ms': float(row['delta_ms']),
                                     'overlapping_commands': overlaps})
    active_start = None
    for row in rows:
        full = row.get('is_redrawing_entirely') == '1' and row.get('is_redraw_active') == '1'
        if full and active_start is None:
            active_start = float(row['t'])
        elif not full and active_start is not None:
            end_t = float(row['t'])
            entry['full_redraw_intervals'].append({'start_t': active_start, 'end_t': end_t,
                                                  'seconds': end_t-active_start, 'completed': True})
            active_start = None
    if active_start is not None:
        end_t = float(rows[-1]['t'])
        entry['full_redraw_intervals'].append({'start_t': active_start, 'end_t': end_t,
                                              'seconds': end_t-active_start, 'completed': False})
    for action in results:
        if action['cmd'] in ('build', 'dismantle', 'full_redraw') and start_t <= action['t_start'] <= stop['t_end']:
            entry['actions'].append({k: action[k] for k in ('cmd', 't_start', 't_end')})
    report.append(entry)
destination = root/'Runs'/args.run_id/'frame_analysis.json'
destination.write_text(json.dumps(report, indent=2), encoding='utf-8')
print(json.dumps(report, indent=2))
