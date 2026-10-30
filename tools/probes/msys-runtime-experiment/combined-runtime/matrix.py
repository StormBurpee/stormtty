"""Fixed component inventory. Missing stages never become a full qualification."""
ORDERS = (('copied', 'combined'), ('combined', 'copied'))
STAGE_CASES = ('raw-1', 'raw-64', 'canonical', 'select-raw',
    'stable-mouse-off', 'stable-mouse-on', 'stable-focus-off', 'stable-focus-on',
    'stable-resize', 'changed-mouse-off', 'changed-mouse-on',
    'prefix-mouse-off', 'prefix-mouse-on', 'storm-mouse-off', 'storm-mouse-on')
# These are required by PLAN.md but intentionally unsupported by stage 1.
PENDING_CASES = ('select-mouse-off', 'select-mouse-on', 'select-focus-off',
    'select-focus-on', 'select-resize',
    'copy-winch-2', 'copy-winch-64', 'copy-external-2', 'copy-external-64',
    'zero-winch', 'zero-external', 'restart-winch-zero', 'restart-winch-positive',
    'restart-external-zero', 'restart-external-positive', 'split-surrogate',
    'mouse-key-ordering')
REQUIRED_CASES = STAGE_CASES + PENDING_CASES


def trials():
    return [(order + 1, case, variant) for order, variants in enumerate(ORDERS)
            for case in STAGE_CASES for variant in variants]


def aggregate(rows):
    expected = trials()
    identities = [(r.get('order'), r.get('case'), r.get('variant')) for r in rows]
    valid_schedule = identities == expected[:len(identities)] and len(identities) <= len(expected)
    complete = valid_schedule and len(identities) == len(expected)
    stage = ('FAIL' if any(r.get('component_verdict') == 'FAIL' for r in rows)
             else 'PASS' if complete and all(r.get('component_verdict') == 'PASS' for r in rows)
             else 'INCONCLUSIVE')
    if not valid_schedule:
        stage = 'INCONCLUSIVE'
    return dict(scope='isolated-component-stage-1', schedule_valid=valid_schedule,
                planned=len(expected), executed=len(rows), stage_verdict=stage,
                full_matrix_verdict='INCONCLUSIVE', pending_cases=list(PENDING_CASES),
                native_qualified=False, runtime_promotion_allowed=False)

EXPECTED = {case: b'b' for case in STAGE_CASES}
EXPECTED.update({'raw-1': b'ab', 'raw-64': b'ab', 'canonical': b'ab\n', 'select-raw': b'ab',
    'stable-focus-on': b'\x1b[Ib', 'prefix-mouse-off': b'ab'})
for case in STAGE_CASES:
    if 'mouse-on' in case:
        EXPECTED[case] = (b'a' if case.startswith('prefix-') else b'') + b'\x1b[<64;2;2Mb'
