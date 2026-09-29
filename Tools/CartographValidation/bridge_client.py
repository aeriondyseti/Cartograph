"""Local CartographTestBridge v1 client. Uses only the Python standard library.

Requests are appended once per ID; results and scenario progress survive client
interruption. A restarted game must separately reject interrupted mutations.
"""
import argparse
import json
import math
import os
import pathlib
import re
import sys
import time
import uuid

ROOT = pathlib.Path(__file__).parent


def read_jsonl(path):
    if not path.exists():
        return []
    rows = []
    for line in path.read_text(encoding='utf-8-sig').splitlines():
        if not line.strip():
            continue
        try:
            rows.append(json.loads(line))
        except json.JSONDecodeError:
            pass  # A writer may still be completing the last line.
    return rows


def send(directory, request, timeout):
    directory.mkdir(parents=True, exist_ok=True)
    results = {row['id']: row for row in read_jsonl(directory/'results.jsonl') if 'id' in row}
    if request['id'] in results:
        return results[request['id']]
    existing = {row['id']: row for row in read_jsonl(directory/'commands.jsonl') if 'id' in row}
    if request['id'] in existing and existing[request['id']] != request:
        raise ValueError('ID already used by a different request')
    if request['id'] not in existing:
        with (directory/'commands.jsonl').open('a', encoding='utf-8', newline='\n') as output:
            output.write(json.dumps(request, separators=(',', ':')) + '\n')
            output.flush()
            os.fsync(output.fileno())
    deadline = time.monotonic() + timeout
    offset = 0
    pending = b''
    while time.monotonic() < deadline:
        path = directory/'results.jsonl'
        if path.exists():
            with path.open('rb') as source:
                source.seek(offset)
                new = source.read()
                offset = source.tell()
            pending += new
            while b'\n' in pending:
                line, pending = pending.split(b'\n', 1)
                if not line.strip():
                    continue
                row = json.loads(line.decode('utf-8-sig'))
                if row.get('id') == request['id']:
                    return row
        time.sleep(0.2)
    raise TimeoutError(f"No completion for {request['id']} after {timeout}s; request retained, do not duplicate mutation")


parser = argparse.ArgumentParser()
parser.add_argument('--dir', type=pathlib.Path, default=ROOT/'Bridge')
parser.add_argument('--timeout', type=float, default=600)
sub = parser.add_subparsers(dest='mode', required=True)
single = sub.add_parser('send')
single.add_argument('command')
single.add_argument('--args', default='{}')
single.add_argument('--id')
scenario = sub.add_parser('run')
scenario.add_argument('scenario', type=pathlib.Path)
scenario.add_argument('--run-id', required=True)
scenario.add_argument('--batch', action='store_true', help='Queue reversible UI checks together for same-frame races; assertions cannot stop commands already queued')
args = parser.parse_args()

if args.mode == 'send':
    request = {'id': args.id or 'astra-' + str(uuid.uuid4()), 'cmd': args.command, **json.loads(args.args)}
    result = send(args.dir, request, args.timeout)
    print(json.dumps(result, indent=2))
    sys.exit(0 if result.get('ok') else 1)

actions = json.loads(args.scenario.read_text(encoding='utf-8-sig'))
if not isinstance(actions, list) or len(actions) > 500:
    raise ValueError('Scenario must be a list of at most 500 commands')
if not re.fullmatch(r'[A-Za-z0-9_-]+', args.run_id):
    raise ValueError('Run ID must contain only letters, digits, underscores or hyphens')
run_dir = ROOT/'Runs'/args.run_id
run_dir.mkdir(parents=True, exist_ok=True)
manifest = run_dir/'scenario.json'
normalized = json.dumps(actions, indent=2)
if manifest.exists() and manifest.read_text(encoding='utf-8') != normalized:
    raise ValueError('Run ID already belongs to a different scenario')
manifest.write_text(normalized, encoding='utf-8')
if args.batch:
    batch_commands = {'ping', 'state', 'mem', 'wait', 'wait_redraw_idle', 'map_open', 'map_close',
                      'map_open_direct', 'map_close_direct', 'full_redraw', 'rt_hash', 'verify_indices'}
    if any(action.get('cmd') not in batch_commands for action in actions):
        raise ValueError('Batch mode is limited to reversible UI and read-only checks')
    args.dir.mkdir(parents=True, exist_ok=True)
    existing = {row['id']: row for row in read_jsonl(args.dir/'commands.jsonl') if 'id' in row}
    pending_requests = []
    for index, action in enumerate(actions):
        wire_action = {k: v for k, v in action.items() if not k.startswith('_')}
        for field in ('name', 'dump'):
            if isinstance(wire_action.get(field), str):
                wire_action[field] = wire_action[field].replace('{run}', args.run_id)
        request = {'id': f'{args.run_id}-{index:03}', **wire_action}
        if request['id'] in existing and existing[request['id']] != request:
            raise ValueError('ID already used by a different request')
        if request['id'] not in existing:
            pending_requests.append(request)
    with (args.dir/'commands.jsonl').open('a', encoding='utf-8', newline='\n') as output:
        output.write(''.join(json.dumps(r, separators=(',', ':')) + '\n' for r in pending_requests))
        output.flush()
        os.fsync(output.fileno())
completed = []
captures = {}
for index, action in enumerate(actions):
    action = dict(action)
    expected = action.pop('_expect', {})
    near = action.pop('_expect_near', {})
    minimums = action.pop('_expect_min', {})
    capture = action.pop('_capture', None)
    same_hash = action.pop('_same_hash_as', None)
    different_hash = action.pop('_different_hash_as', None)
    same_data = action.pop('_same_data_as', None)
    delta_from = action.pop('_expect_delta', None)
    for placeholder_field in ('name', 'dump'):
        if placeholder_field in action and isinstance(action[placeholder_field], str):
            action[placeholder_field] = action[placeholder_field].replace('{run}', args.run_id)
    request = {'id': f'{args.run_id}-{index:03}', **action}
    (ROOT/'phase.txt').write_text(f"{request['id']}:{request['cmd']}", encoding='utf-8')
    result = send(args.dir, request, args.timeout)
    completed.append(result)
    (run_dir/'results.json').write_text(json.dumps(completed, indent=2), encoding='utf-8')
    print(json.dumps({key: result.get(key) for key in ('id', 'cmd', 'ok', 'error', 't_start', 't_end')}), flush=True)
    if not result.get('ok'):
        print(json.dumps(result, indent=2), flush=True)
        sys.exit(1)
    payload = result.get('data', {})
    for key, expected_value in expected.items():
        if payload.get(key) != expected_value:
            raise AssertionError(f"{request['id']}: {key} expected {expected_value!r}, got {payload.get(key)!r}")
    for key, spec in near.items():
        actual, target = payload.get(key), spec['value']
        actual_values = actual if isinstance(actual, list) else [actual]
        target_values = target if isinstance(target, list) else [target]
        tolerance = float(spec['tolerance'])
        if not math.isfinite(tolerance) or tolerance < 0:
            raise ValueError('Tolerance must be finite and nonnegative')
        if len(actual_values) != len(target_values) or any(
            not isinstance(x, (int, float)) or not isinstance(y, (int, float))
            or not math.isfinite(x) or not math.isfinite(y) or abs(x-y) > tolerance
            for x, y in zip(actual_values, target_values)
        ):
            raise AssertionError(f"{request['id']}: {key} expected {target!r} within {tolerance}, got {actual!r}")
    for key, minimum in minimums.items():
        if payload.get(key) is None or payload[key] < minimum:
            raise AssertionError(f"{request['id']}: {key} expected at least {minimum!r}, got {payload.get(key)!r}")
    if same_hash:
        reference = captures[same_hash]
        for key in ('width', 'height', 'hash'):
            if payload.get(key) != reference.get(key):
                raise AssertionError(f"{request['id']}: {key} differs from {same_hash}: {payload.get(key)} != {reference.get(key)}")
    if same_data and payload != captures[same_data]:
        raise AssertionError(f"{request['id']}: data differs from {same_data}; inspect persisted results")
    if different_hash and payload.get('hash') == captures[different_hash].get('hash'):
        raise AssertionError(f"{request['id']}: hash unexpectedly unchanged from {different_hash}")
    if delta_from:
        reference = captures[delta_from['capture']]
        for key, delta in delta_from['fields'].items():
            expected_value = reference[key] + delta
            if payload.get(key) != expected_value:
                raise AssertionError(f"{request['id']}: {key} expected {expected_value} after delta {delta}, got {payload.get(key)}")
    if capture:
        captures[capture] = payload
print(f'Completed {len(completed)} commands; results in {run_dir}', flush=True)
