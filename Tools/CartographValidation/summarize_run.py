"""Summarize persisted bridge results and external memory samples, without game I/O."""
import argparse
import csv
import datetime as dt
import json
import pathlib
import statistics

root = pathlib.Path(__file__).parent
parser = argparse.ArgumentParser()
parser.add_argument('run_id')
parser.add_argument('--memory', type=pathlib.Path)
args = parser.parse_args()
results = json.loads((root/'Runs'/args.run_id/'results.json').read_text())
report = {'run_id': args.run_id, 'commands_completed': len(results), 'commands': []}
for row in results:
    item = {'id': row['id'], 'cmd': row['cmd'], 'ok': row['ok'],
            'seconds': row['t_end'] - row['t_start']}
    if row['cmd'] in ('sample_stop', 'mem', 'state', 'rt_hash', 'verify_indices', 'save'):
        item['data'] = row['data']
    if row.get('error'):
        item['error'] = row['error']
    report['commands'].append(item)

if args.memory:
    with args.memory.open(encoding='utf-8-sig', newline='') as source:
        samples = list(csv.DictReader(source))
    metrics = ('private_bytes', 'working_set_bytes', 'gpu_dedicated_bytes',
               'gpu_shared_bytes', 'gpu_committed_bytes')
    report['external_memory_by_phase'] = []
    for command in results:
        phase = command['id'] + ':' + command['cmd']
        selected = [x for x in samples if x['phase'] == phase]
        if not selected:
            continue
        for sample in selected:
            sample['_time'] = dt.datetime.fromisoformat(sample['utc'].replace('Z', '+00:00'))
        end = max(x['_time'] for x in selected)
        tail = [x for x in selected if (end - x['_time']).total_seconds() <= 10]
        entry = {'phase': phase, 'samples': len(selected),
                 'pids': sorted({x['pid'] for x in selected}), 'last_10s_samples': len(tail),
                 'max_bytes': {}, 'last_10s_median_bytes': {}}
        for metric in metrics:
            values = [int(x[metric]) for x in selected if x.get(metric)]
            tail_values = [int(x[metric]) for x in tail if x.get(metric)]
            entry['max_bytes'][metric] = max(values) if values else None
            entry['last_10s_median_bytes'][metric] = statistics.median(tail_values) if tail_values else None
        report['external_memory_by_phase'].append(entry)

destination = root/'Runs'/args.run_id/'summary.json'
destination.write_text(json.dumps(report, indent=2), encoding='utf-8')
print(destination)
