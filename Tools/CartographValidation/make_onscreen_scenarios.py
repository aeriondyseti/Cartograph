"""Generate reproducible visible-construction scenarios from an approved view.

Does not submit commands. Inspect placement in game before running measurements.
The view file is a bridge `view` result or its `data` object. Save name and user
coordinates are supplied locally rather than embedded in reusable source.
"""
import argparse
import json
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument('--save', required=True)
p.add_argument('--view', type=Path, required=True)
p.add_argument('--out', type=Path, required=True)
p.add_argument('--layout', type=Path, help='Approved explicit grid origin/yaw/spacing/snap_to_ground JSON')
a = p.parse_args()
v = json.loads(a.view.read_text(encoding='utf-8-sig'))
v = v.get('data', v)
origin = v['player_location']
yaw, pitch = v['control_yaw'], v['control_pitch']
layout = json.loads(a.layout.read_text(encoding='utf-8-sig')) if a.layout else {
    'origin': origin, 'yaw': yaw, 'offset': [2000, -3600, 0],
    'spacing': 2400, 'snap_to_ground': True,
}
if v.get('player_class') != 'Char_Player_C' or len(origin) != 3:
    raise ValueError('Need a real loaded player view, not the menu/fresh default pawn')

recipes = {
    'foundry': '/Game/FactoryGame/Recipes/Buildings/Recipe_SmelterMk1.Recipe_SmelterMk1_C',
    'assembler': '/Game/FactoryGame/Recipes/Buildings/Recipe_AssemblerMk1.Recipe_AssemblerMk1_C',
}


def idle():
    return {'cmd': 'wait_redraw_idle', 'timeout': 300, 'settle_frames': 5}


def build(kind, count=16, columns=4, spacing=None):
    return dict(cmd='build', recipe=recipes[kind], count=count, rate=1,
                origin=layout['origin'], yaw=layout['yaw'], offset=layout.get('offset', [0, 0, 0]),
                spacing=layout['spacing'] if spacing is None else spacing,
                columns=columns, snap_to_ground=layout['snap_to_ground'],
                group='onscreen_' + kind,
                _expect={'spawned': count, **({'no_ground_found': 0} if layout['snap_to_ground'] else {})})


def setup():
    expected_view = {key: v[key] for key in (
        'player_class', 'viewport_width', 'viewport_height', 'window_mode',
        'r.ScreenPercentage', 'r.VSync', 't.MaxFPS')}
    return [
        {'cmd': 'load_save', 'name': a.save, 'method': 'manager'},
        {'cmd': 'wait_world', 'timeout': 300},
        {'cmd': 'wait_init_done', 'timeout': 300},
        {'cmd': 'set_view', 'yaw': yaw, 'pitch': pitch},
        {'cmd': 'wait', 'seconds': 1},
        {'cmd': 'view', '_expect': expected_view,
         '_expect_near': {'player_location': {'value': origin, 'tolerance': 25}}},
    ]


preflight = setup() + [build('foundry', 4, 2, layout['spacing'] * 3), idle(), {'cmd': 'view'}]
# Intentionally leave four corner markers for a visual check; separate cleanup.
cleanup = [{'cmd': 'dismantle', 'group': 'onscreen_foundry', 'rate': 4,
            '_expect': {'removed': 4, 'not_found': 0}}, idle()]
measure = setup() + [
    {'cmd': 'map_open'}, idle(), {'cmd': 'map_close'},
    # Reference UI close leaves visibility stuck. Normalize only BEFORE sampling.
    {'cmd': 'map_close_direct'}, idle(),
    {'cmd': 'state', '_capture': 'original'},
    {'cmd': 'sample_start', 'name': '{run}_idle'},
    {'cmd': 'wait', 'seconds': 20},
    {'cmd': 'sample_stop', 'name': '{run}_idle'},
]
for kind in recipes:
    sample = '{run}_' + kind
    measure += [
        {'cmd': 'sample_start', 'name': sample},
        {'cmd': 'map_open'}, {'cmd': 'wait', 'seconds': 0.15},
        {'cmd': 'state', '_expect': {'is_redraw_active': True}},
        {'cmd': 'map_close'},
        # No direct-close workaround in this measured user-facing path.
        build(kind), idle(), {'cmd': 'wait', 'seconds': 5},
        {'cmd': 'dismantle', 'group': 'onscreen_' + kind, 'rate': 1,
         '_expect': {'removed': 16, 'not_found': 0}},
        idle(), {'cmd': 'wait', 'seconds': 5},
        {'cmd': 'sample_stop', 'name': sample},
        {'cmd': 'view', '_expect_near': {'player_location': {'value': origin, 'tolerance': 25}}},
        {'cmd': 'state', '_expect_delta': {
            'capture': 'original', 'fields': {'building_count': 0, 'drawn_building_count': 0}}},
        {'cmd': 'verify_indices', '_expect': {'error_count': 0}},
        # Cleanup outside samples, restoring known state for the next case.
        {'cmd': 'map_close_direct'}, idle(),
    ]
measure += [{'cmd': 'mem'}]
a.out.mkdir(parents=True, exist_ok=True)
for name, actions in [('preflight', preflight), ('preflight_cleanup', cleanup), ('measure', measure)]:
    target = a.out / f'onscreen_{name}.json'
    target.write_text(json.dumps(actions, indent=2) + '\n', encoding='utf-8')
    print(f'{target}: {len(actions)} commands (not submitted)')
