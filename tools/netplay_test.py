"""Online play test: two copies of the game on this PC, linked through a (simulated) bad connection.

Each copy gets its own script of random controls. A third, ordinary run is given both scripts merged (each
shifted by the input delay, as online play applies them). All three must end with the same memory
fingerprint, and the two linked copies must agree every time they compare memory on the way.

    python tools/netplay_test.py [--frames N] [--delay D] [--sim "ms,jitter,loss%"] [--sim2 ...] [--seed S]
                                 [--fast] [--windowed] [--exe PATH]

--sim is the host's outgoing connection and --sim2 the joiner's (default: the same). --fast runs the linked
copies as fast as they can instead of at 60 frames a second (a harsher test of the waiting logic).
"""
import argparse, os, random, re, subprocess, sys, time

ap = argparse.ArgumentParser()
ap.add_argument('--frames', type=int, default=5000)
ap.add_argument('--delay', type=int, default=2)
ap.add_argument('--sim', default='')
ap.add_argument('--sim2', default=None)
ap.add_argument('--seed', type=int, default=1)
ap.add_argument('--port', type=int, default=7611)
ap.add_argument('--fast', action='store_true')
ap.add_argument('--exe', default=None)
ap.add_argument('--no-solo', action='store_true')
ap.add_argument('--windowed', action='store_true', help='two real windows (no ordinary run to compare with: the clock and save memory are live)')
a = ap.parse_args()

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
exe = os.path.abspath(a.exe) if a.exe else os.path.join(root, 'recomp', 'buriki.exe')
cwd = os.path.dirname(exe)
out = os.path.join(root, 'recomp', 'nettest')
os.makedirs(out, exist_ok=True)

BUTTONS = ['b1', 'b2', 'b3', 'b4']
DIRS = ['up', 'down', 'left', 'right']


def make_steps(rng, coin_at, start_at, frames):
    """[(frame, set of controls)]: a coin, start, then random stick and buttons changing every few frames."""
    steps = [(coin_at, {'coin'}), (coin_at + 6, set()), (start_at, {'start'}), (start_at + 6, set())]
    f = start_at + 60
    while f < frames:
        held = set()
        r = rng.random()
        if r < 0.15:
            pass
        else:
            if rng.random() < 0.6:
                held.add(rng.choice(DIRS))
            if rng.random() < 0.7:
                held.add(rng.choice(BUTTONS))
            if rng.random() < 0.15:
                held.add(rng.choice(BUTTONS))
        if rng.random() < 0.02:
            held.add('start')
        steps.append((f, held))
        f += rng.randint(1, 14)
    return steps


def write_script(path, timeline):
    with open(path, 'w') as fh:
        fh.write(';'.join('%d=%s' % (f, ','.join(sorted(h))) for f, h in timeline))


def local_script(steps):
    return [(f, {'p1' + c for c in held}) for f, held in steps]


def merged_script(host, join, delay):
    frames = sorted({f for f, _ in host} | {f for f, _ in join})
    res, hi, ji, hh, jh = [], 0, 0, set(), set()
    for f in frames:
        while hi < len(host) and host[hi][0] <= f:
            hh = host[hi][1]; hi += 1
        while ji < len(join) and join[ji][0] <= f:
            jh = join[ji][1]; ji += 1
        res.append((f + delay, {'p1' + c for c in hh} | {'p2' + c for c in jh}))
    return res


rng = random.Random(a.seed)
host_steps = make_steps(rng, 1300, 1400, a.frames)
join_steps = make_steps(rng, 1500, 1560, a.frames)
paths = {k: os.path.join(out, k + '.txt') for k in ('host', 'join', 'merged')}
write_script(paths['host'], local_script(host_steps))
write_script(paths['join'], local_script(join_steps))
write_script(paths['merged'], merged_script(host_steps, join_steps, a.delay))

base = [exe] + (['--scale', '1'] if a.windowed else ['--headless']) + ['--no-sound', '--frames', str(a.frames)]
if a.windowed:
    a.no_solo = True


def start(name, extra, sim=None):
    env = dict(os.environ)
    env.pop('BURIKI_NET_SIM', None)
    if sim:
        env['BURIKI_NET_SIM'] = sim
    log = open(os.path.join(out, name + '.log'), 'w')
    return subprocess.Popen(base + extra, cwd=cwd, stderr=log, stdout=log, env=env), log


def result(name):
    text = open(os.path.join(out, name + '.log'), errors='replace').read()
    m = re.search(r'\[stop\] reached (\d+) frames, RAM fingerprint ([0-9A-F]+)', text)
    return (m.group(2) if m else None), [l for l in text.splitlines() if l.startswith('[netplay]')], text


t0 = time.time()
procs = []
if not a.no_solo:
    procs.append(start('solo', ['--input-script', '@' + paths['merged']]))
pace = ['--no-throttle'] if a.fast else []
host_nv = ['--nvram', os.path.join(out, 'host_nvram.bin')] if a.windowed else []
procs.append(start('host', pace + host_nv + ['--input-script', '@' + paths['host'], '--netplay-host', str(a.port), '--netplay-delay', str(a.delay)], a.sim))
time.sleep(0.5)
procs.append(start('join', pace + ['--input-script', '@' + paths['join'], '--netplay-join', '127.0.0.1:%d' % a.port],
                   a.sim if a.sim2 is None else a.sim2))
limit = a.frames / 50.0 + 120
for p, log in procs:
    try:
        p.wait(timeout=max(5, limit - (time.time() - t0)))
    except subprocess.TimeoutExpired:
        p.kill()
    log.close()

names = ([] if a.no_solo else ['solo']) + ['host', 'join']
prints = {}
for nme in names:
    fp, lines, text = result(nme)
    prints[nme] = fp
    print('%-5s fingerprint %s' % (nme, fp))
    for l in lines:
        if 'rollbacks' in l or 'sound:' in l or 'memory compared' in l or 'OUT OF STEP' in l or 'not settled' in l or 'lost' in l:
            print('      ' + l)
    if fp is None:
        print('      (did not finish) last lines:')
        for l in text.splitlines()[-4:]:
            print('      | ' + l)
ok = len(set(prints.values())) == 1 and None not in prints.values()
bad_checks = any('OUT OF STEP' in result(nme)[2] for nme in ('host', 'join'))
print('RESULT: %s  (%.0f s; frames %d, delay %d, sim "%s" / "%s"%s)' % (
    'PASS' if ok and not bad_checks else 'FAIL', time.time() - t0, a.frames, a.delay, a.sim,
    a.sim if a.sim2 is None else a.sim2, (', fast' if a.fast else '') + (', windowed' if a.windowed else '')))
sys.exit(0 if ok and not bad_checks else 1)
