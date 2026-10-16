#!/usr/bin/env python3
"""Runs one test environment's suites and checks its bars (TESTING.md).

    python3 tools/run_env.py ENV [--seed N] [--jobs N] [--scale X]
                             [--showdown-commit HASH | --master]
                             [--out DIR] [--keep-traces]

ENV is a table in tools/criteria.toml, with team files (`teams`) or a pool
that tools/gen_teams.cjs draws from the run seed (`generate`). The suites, each split into --jobs
shards run in parallel: check_showdown (traces from Showdown, then the
check), soak (debug) and check_chance (release). A fresh 64-bit seed is
drawn unless --seed is given; every shard's seed derives from it, so the run
replays from the printed seed. --scale multiplies every game count (0.01 for
a smoke run; the rates are noisy then).

Showdown is the commit in SHOWDOWN_COMMIT (the gating run), or another
(--showdown-commit, or --master for the reporting run), kept built in
.showdown/<hash>/. The `showdown` symlink points at the pinned one.

Exits 0 when every bar passes, 1 when one fails, 2 on a setup error.
"""
import argparse
import concurrent.futures as cf
import datetime
import glob
import math
import os
import re
import secrets
import subprocess
import sys
import time
import threading
import tomllib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SHOWDOWN_URL = 'https://github.com/smogon/pokemon-showdown.git'
MASK64 = (1 << 64) - 1


def splitmix64(state):
    """The shard seeds: SplitMix64's outputs from the run seed."""
    while True:
        state = (state + 0x9E3779B97F4A7C15) & MASK64
        z = state
        z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & MASK64
        z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & MASK64
        yield z ^ (z >> 31)


def run(cmd, **kw):
    return subprocess.run(cmd, cwd=ROOT, check=True, **kw)


def git(*args, cwd=ROOT):
    return subprocess.run(['git', *args], cwd=cwd, check=True,
                          capture_output=True, text=True).stdout.strip()


def showdown_checkout(commit):
    """The built Showdown at `commit`, fetched and built on first use."""
    path = os.path.join(ROOT, '.showdown', commit)
    if os.path.exists(os.path.join(path, 'dist', 'sim', 'index.js')):
        return path
    print(f'fetching and building Showdown {commit} in {path}', flush=True)
    os.makedirs(path, exist_ok=True)
    git('init', '-q', cwd=path)
    git('fetch', '-q', '--depth', '1', SHOWDOWN_URL, commit, cwd=path)
    git('checkout', '-q', 'FETCH_HEAD', cwd=path)
    subprocess.run(['npm', 'ci', '--no-audit', '--no-fund'], cwd=path,
                   check=True, stdout=subprocess.DEVNULL)
    subprocess.run(['node', 'build'], cwd=path, check=True,
                   stdout=subprocess.DEVNULL)
    return path


def link_pinned(path):
    """Points the `showdown` symlink at the pinned checkout. A real
    directory there is left alone."""
    link = os.path.join(ROOT, 'showdown')
    target = os.path.relpath(path, ROOT)
    if os.path.islink(link):
        if os.readlink(link) == target:
            return
        os.remove(link)
    elif os.path.exists(link):
        print(f'warning: {link} is not a symlink; left as it is')
        return
    os.symlink(target, link)


def build():
    for preset in ('release', 'debug'):
        if not os.path.exists(os.path.join(ROOT, 'build', preset)):
            run(['cmake', '--preset', preset], stdout=subprocess.DEVNULL)
        run(['cmake', '--build', '--preset', preset, '-j', str(os.cpu_count())],
            stdout=subprocess.DEVNULL)


def split(total, n):
    """`total` games over `n` shards, as evenly as possible."""
    return [total // n + (i < total % n) for i in range(n)]


# Each shard job returns (suite, shard, seed, returncode, output, extra).

# Showdown's Node processes hold about 350 MB by 250 games and keep growing,
# so 16 at once can exhaust a laptop's memory (it did: the OOM killer took
# the user's browser). They get their own limit, from the memory free at
# the start; the C++ jobs take 5-25 MB each.
TRACE_BUDGET = 1 << 30
traces_running = None


def default_trace_jobs(jobs):
    try:
        with open('/proc/meminfo') as f:
            free = next(int(l.split()[1]) * 1024 for l in f
                        if l.startswith('MemAvailable:'))
    except (OSError, StopIteration):
        return min(jobs, 4)
    return max(1, min(jobs, free // TRACE_BUDGET))


def showdown_shard(i, seed, games, crit, files, sd, out, keep):
    trace = os.path.join(out, f'traces{i}.jsonl')
    with traces_running, open(trace, 'w') as f:
        t = subprocess.run(['node', 'tools/showdown_traces.cjs', '--games',
                            str(games), '--seed', str(seed), '--showdown', sd,
                            *files], cwd=ROOT, stdout=f, stderr=subprocess.PIPE,
                           text=True)
    if t.returncode:
        return ('showdown', i, seed, t.returncode, t.stderr, trace)
    c = subprocess.run(['build/release/check_showdown', '--quiet', '--tries',
                        str(crit['tries']), '--seed', str(seed), trace],
                       cwd=ROOT, capture_output=True, text=True)
    failed = c.returncode != 0 or '\ngame ' in '\n' + c.stdout
    if not keep and not failed:
        os.remove(trace)
    return ('showdown', i, seed, c.returncode, c.stdout, trace)


def soak_shard(i, seed, games, files):
    c = subprocess.run(['build/debug/soak', '--games', str(games), '--seed',
                        str(seed), *files], cwd=ROOT, capture_output=True,
                       text=True)
    return ('soak', i, seed, c.returncode, c.stdout + c.stderr, None)


def chance_shard(i, seed, games, crit, files):
    c = subprocess.run(['build/release/check_chance', '--games', str(games),
                        '--samples', str(crit['samples']), '--seed', str(seed),
                        *files], cwd=ROOT, capture_output=True, text=True)
    return ('chance', i, seed, c.returncode, c.stdout + c.stderr, None)


SHOWDOWN_SUMMARY = re.compile(
    r'check_showdown: (\d+) games, (\d+) matched to the end, (\d+) left out at '
    r'a multi-hit step; .* failures: (\d+) setup, (\d+) choices, (\d+) '
    r'unmatched \((\d+) by the log alone\)')
SOAK_SUMMARY = re.compile(r'soak: (\d+) games.*\n.*failures (\d+)')
CHANCE_SUMMARY = re.compile(r'check_chance: .* (\d+) collisions')
CHANCE_PROB = re.compile(r'probability: (\d+) sum failures, (\d+) frequency')
# The label is padded ("2^-1 ..2^-0").
CHANCE_BIN = re.compile(
    r'^\s+(2\^-\d+\s*\.\.2\^-\d+|unseen)\s+(\S+) /\s+\S+\s+p\s+\S+\s+\[(\S+) (\S+)\]',
    re.M)


def bin_tail(o, e, v):
    """check_chance's bin_tail: a normal tail for a large variance, else
    Poisson(e), on o's side."""
    if v >= 100:
        return 0.5 * math.erfc(abs(o - e) / math.sqrt(2 * v))
    if e <= 0:
        return 0.0 if o > 0 else 1.0
    def pmf(k):
        return math.exp(k * math.log(e) - e - math.lgamma(k + 1))
    total = 0.0
    if o > e:
        k = o
        while k < o + 1e6:
            t = pmf(k)
            total += t
            if t <= total * 1e-17:
                break
            k += 1
    else:
        total = sum(pmf(k) for k in range(int(o) + 1))
    return min(total, 1.0)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('env')
    ap.add_argument('--seed', type=int)
    ap.add_argument('--jobs', type=int, default=os.cpu_count())
    ap.add_argument('--trace-jobs', type=int,
                    help='Node trace processes at once (default: 1 per GB free)')
    ap.add_argument('--scale', type=float, default=1.0)
    ap.add_argument('--showdown-commit')
    ap.add_argument('--master', action='store_true')
    ap.add_argument('--out')
    ap.add_argument('--keep-traces', action='store_true')
    args = ap.parse_args()

    with open(os.path.join(ROOT, 'tools', 'criteria.toml'), 'rb') as f:
        criteria = tomllib.load(f)
    if args.env not in criteria:
        print(f'run_env: no environment {args.env!r} in tools/criteria.toml '
              f'(have: {", ".join(criteria)})')
        return 2
    env = criteria[args.env]
    if 'teams' in env:
        files = sorted(glob.glob(os.path.join(ROOT, env['teams'], '*.txt')))
        if not files:
            print(f'run_env: no team files in {env["teams"]}')
            return 2
    elif 'generate' not in env:
        print(f'run_env: {args.env} has neither teams nor generate')
        return 2

    with open(os.path.join(ROOT, 'SHOWDOWN_COMMIT')) as f:
        pinned = f.read().strip()
    if args.master:
        commit = subprocess.run(['git', 'ls-remote', SHOWDOWN_URL, 'master'],
                                check=True, capture_output=True,
                                text=True).stdout.split()[0]
    else:
        commit = args.showdown_commit or pinned
    sd = showdown_checkout(commit)
    if commit == pinned:
        link_pinned(sd)
    build()

    seed = args.seed if args.seed is not None else secrets.randbits(64)
    engine = git('rev-parse', 'HEAD')
    dirty = bool(git('status', '--porcelain', '--untracked-files=no'))
    stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    out = args.out or os.path.join(ROOT, 'build', 'runs',
                                   f'{args.env}-{stamp}-{seed}')
    os.makedirs(out, exist_ok=True)
    header = (f'run_env {args.env}: seed {seed}, engine {engine}'
              f'{" (dirty)" if dirty else ""}, Showdown {commit}'
              f'{" (pinned)" if commit == pinned else ""}, scale {args.scale}, '
              f'{args.jobs} jobs, output {out}')
    if not args.trace_jobs:
        header += f' (trace jobs: {default_trace_jobs(max(1, args.jobs))})'
    print(header, flush=True)

    start = time.monotonic()
    n = max(1, args.jobs)
    global traces_running
    trace_jobs = args.trace_jobs or default_trace_jobs(n)
    traces_running = threading.Semaphore(trace_jobs)
    seeds = splitmix64(seed)
    if 'generate' in env:
        # A fresh pool from the run seed, read by every suite.
        gen = env['generate']
        files = [os.path.join(out, 'teams.txt')]
        with open(files[0], 'w') as f:
            run(['node', 'tools/gen_teams.cjs', '--mode', gen['mode'], '--teams',
                 str(gen['teams']), '--seed', str(next(seeds)), '--showdown', sd],
                stdout=f, stderr=subprocess.DEVNULL)
        print(f'  {gen["teams"]} {gen["mode"]} teams in {files[0]}', flush=True)
    scaled = lambda g: max(1, round(g * args.scale))
    jobs = []
    with cf.ThreadPoolExecutor(max_workers=n) as pool:
        sdc = env['showdown']
        for i, g in enumerate(split(scaled(sdc['games']), n)):
            if g:
                jobs.append(pool.submit(showdown_shard, i, next(seeds), g, sdc,
                                        files, sd, out, args.keep_traces))
        for i, g in enumerate(split(scaled(env['soak']['games']), n)):
            if g:
                jobs.append(pool.submit(soak_shard, i, next(seeds), g, files))
        cc = env['chance']
        for i, g in enumerate(split(scaled(cc['games']), n)):
            if g:
                jobs.append(pool.submit(chance_shard, i, next(seeds), g, cc,
                                        files))
        results = []
        for done, job in enumerate(cf.as_completed(jobs), 1):
            r = job.result()
            results.append(r)
            print(f'  [{done}/{len(jobs)}] {r[0]} shard {r[1]} done at '
                  f'{time.monotonic() - start:.0f} s', flush=True)

    lines, bars = [header], []

    def bar(name, ok, detail):
        bars.append(ok)
        lines.append(f'{"PASS" if ok else "FAIL"}  {name}: {detail}')

    # Each shard's output goes to a file; failures are quoted in the summary.
    notes = []
    for suite, i, s, code, text, trace in sorted(results, key=lambda r: r[:2]):
        with open(os.path.join(out, f'{suite}{i}.txt'), 'w') as f:
            f.write(text)

    # check_showdown
    tot = dict(games=0, completed=0, multi=0, setup=0, choices=0, unmatched=0,
               log_only=0)
    crashed = 0
    for suite, i, s, code, text, trace in results:
        if suite != 'showdown':
            continue
        m = SHOWDOWN_SUMMARY.search(text)
        if not m:
            crashed += 1
            notes.append(f'showdown shard {i} (seed {s}) has no summary, exit '
                         f'{code}: see {out}/showdown{i}.txt')
            continue
        for k, v in zip(tot, map(int, m.groups())):
            tot[k] += v
        for line in text.splitlines():
            # With --quiet, every game line is a failure.
            if line.startswith('game ') and 'multi-hit' not in line:
                g = re.match(r'game (\d+)', line)
                replay = (f' (replay: build/release/check_showdown --game '
                          f'{g.group(1)} --seed {s} {trace})') if g else ''
                notes.append(f'showdown shard {i}: {line.strip()}{replay}')
    sdc = env['showdown']
    counted = tot['games'] - tot['multi']
    matched = tot['completed'] + (0 if sdc['log_only_counts'] else tot['log_only'])
    rate = matched / counted if counted else 0.0
    bar('check_showdown matched', crashed == 0 and rate >= sdc['matched'],
        f'{matched}/{counted} = {100 * rate:.2f}% (bar {100 * sdc["matched"]:.2f}%), '
        f'{tot["multi"]} multi-hit games left out, {tot["unmatched"]} unmatched '
        f'({tot["log_only"]} by the log alone{"" if sdc["log_only_counts"] else ", not counted"}), '
        f'{tot["setup"]} setup, {crashed} shards without a summary')
    bar('check_showdown choice sets', crashed == 0 and tot['choices'] <= sdc['choices'],
        f'{tot["choices"]} mismatches (bar {sdc["choices"]})')

    # soak
    games = fails = crashed = 0
    for suite, i, s, code, text, _ in results:
        if suite != 'soak':
            continue
        m = SOAK_SUMMARY.search(text)
        if m:
            games += int(m.group(1))
            fails += int(m.group(2))
        if code != 0 or not m:
            crashed += not m
            tail = '\n      '.join(text.strip().splitlines()[-6:])
            notes.append(f'soak shard {i} (seed {s}) exit {code}:\n      {tail}')
    bar('soak', fails == 0 and crashed == 0,
        f'{games} games, {fails} failures, {crashed} shards crashed')

    # check_chance
    coll = mass = freq = crashed = 0
    bins = {}
    for suite, i, s, code, text, _ in results:
        if suite != 'chance':
            continue
        m, p = CHANCE_SUMMARY.search(text), CHANCE_PROB.search(text)
        if not m or not p:
            crashed += 1
            notes.append(f'chance shard {i} (seed {s}) has no summary, exit '
                         f'{code}: see {out}/chance{i}.txt')
            continue
        coll += int(m.group(1))
        mass += int(p.group(1))
        freq += int(p.group(2))
        for name, o, e, v in CHANCE_BIN.findall(text):
            b = bins.setdefault(re.sub(r'\s', '', name), [0.0, 0.0, 0.0])
            b[0] += float(o)
            b[1] += float(e)
            b[2] += float(v)
        for line in text.splitlines():
            if line.startswith('game ') and ('collision' in line or 'key seen' in line
                                             or 'sum to' in line):
                g = re.match(r'game (\d+)', line).group(1)
                notes.append(f'chance shard {i}: {line.strip()} (replay: '
                             f'build/release/check_chance --samples '
                             f'{env["chance"]["samples"]} --replay {g} '
                             f'{" ".join(os.path.relpath(f, ROOT) for f in files)})')
    pmin, worst = 1.0, ''
    for name, (o, e, v) in bins.items():
        p = bin_tail(o, e, v)
        if p < pmin:
            pmin, worst = p, name
    bar('check_chance collisions', coll == 0 and crashed == 0,
        f'{coll} collisions, {crashed} shards without a summary')
    bar('check_chance probabilities', mass == 0 and freq == 0
        and pmin >= env['chance']['alpha'],
        f'{mass} sum failures, {freq} frequency failures, smallest bin tail '
        f'{pmin:.2g} ({worst}; bar {env["chance"]["alpha"]:g})')

    if notes:
        lines.append('failures:')
        lines += ['  ' + n for n in notes]
    ok = all(bars)
    lines.append(f'run_env {args.env}: {"PASS" if ok else "FAIL"} (seed {seed})')
    text = '\n'.join(lines)
    print(text)
    with open(os.path.join(out, 'summary.txt'), 'w') as f:
        f.write(text + '\n')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
