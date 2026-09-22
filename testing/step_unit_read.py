#!/usr/bin/env python3
"""Read solver/step_unit.h's ladder out of whatever this project's drivers wrote.

  python3 testing/step_unit_read.py <path...>            one line per artifact, OLDEST FIRST
  python3 testing/step_unit_read.py --hist <path...>     the full per-arm table, terminal census
  python3 testing/step_unit_read.py --marginal <path...> the cost BETWEEN consecutive censuses of one run

WHY IT IS A TRACKED FILE RATHER THAN A SCRATCH SCRIPT.  The rows below had a writer in the
engine and no reader in any tracked driver, so every number ever quoted about them came
out of an ad-hoc script in a container that is reclaimed — a figure nobody can re-derive.
CLAUDE.md: hand over the DERIVATION AS A COMMAND, never the number.  It therefore prints
NO expected total and NO expected count of its own; both rot, and a count of what is
missing shrinks as the work is done.  Its arm names come from the histogram's own keys and
are never listed here, because a second list of solver/step_unit.h's arms would drift.

WHAT IT READS.  Its tracked input is `testing/live-run.js`'s result document, whose
`counters[i]` and `frontier[i]` are ALIGNED INDEX FOR INDEX and come from one composed
result document — which is what makes pairing them legitimate and is the ONLY pairing this
file makes.  It also walks any other JSON carrying the rows (an ad-hoc probe, a JSONL
sample stream) and scans `@COLD ` lines out of a native smoke log.

THE KINDS, READ OFF THE ACCESSORS IN solver/engine.h AND NOT OFF THE NAMES:
  steps, sliceOverruns, sliceUs, stepUnitRuns{}, stepUnitOverruns{},
  unitMidProgram, unitParked, unitCheckpointOwed, classicCompiles,
  classicCompileOverruns, finished        LIFETIME counts over the AGENT's life.
                                          solver/result.c: reset by nothing at all, so
                                          differencing two censuses of ONE instance is
                                          legitimate and differencing across instances is
                                          not.  This file checks monotonicity and says so.
  deepest, completed, deepestLeft         MAXIMA.  A maximum saturates and then plateaus,
                                          and a plateau is not a ceiling; the length of
                                          the series is part of quoting one.
  stepUnits{}, programCursors{}           GAUGES.  Never differenced here.

THE IDENTITIES IT CHECKS, each asserted in the engine where its terms are in one hand:
  sum(stepUnitRuns)      == steps
  sum(stepUnitOverruns)  == sliceOverruns
  unitMidProgram + unitParked + unitCheckpointOwed + unitsDone == steps
The last spans the result document's TWO objects.  solver/result.c says the assert in
solver/engine.c is the only thing that makes a reader compose it at all, so this file
composes it ONLY where both halves came from one document and prints `n/a` otherwise —
a native `@COLD` line and the `_unitsDone` of some later result document are two instants.

--marginal IS THE READING A TERMINAL CENSUS CANNOT GIVE.  `sliceOverruns` is a COUNT and
solver/engine.h says why it is not a time; the consequence is that the count alone cannot
tell a turn just past the slice from one three orders of magnitude past it.  Between two
consecutive censuses of one run the published rows bound it:

  floor_per_overrun = (d_sliceUs - (d_steps - d_overruns) * sliceMs * 1000) / d_overruns

charges every non-overrunning turn the FULL slice, so it is a LOWER bound.  Negative means
the non-overrunning turns alone could account for the interval — the overruns in it were
marginal.  On a host whose measure is WALL (quantum.isCpu false) descheduling is the one
confound and it can only push this bound UP, never down, so a LARGE floor survives it and
a small one is not evidence of anything.  Read the artifact's own `quantum` row first.
"""
import json, os, re, sys, datetime

TOK = 'unitMidProgram'


def _brace_obj(s, start):
    depth = 0
    i = start
    instr = False
    esc = False
    while i < len(s):
        ch = s[i]
        if instr:
            if esc:
                esc = False
            elif ch == '\\':
                esc = True
            elif ch == '"':
                instr = False
        else:
            if ch == '"':
                instr = True
            elif ch == '{':
                depth += 1
            elif ch == '}':
                depth -= 1
                if depth == 0:
                    return s[start:i + 1]
        i += 1
    return None


def _walk(node, done, quantum, acc):
    """collect (unitsDone, census, quantum) in document order, unitsDone from the
    NEAREST ENCLOSING object so the two halves come from one composed document."""
    if isinstance(node, dict):
        d = node.get('unitsDone', node.get('_unitsDone', done))
        q = node.get('quantum', node.get('_quantum', quantum))
        if TOK in node:
            acc.append((d, node, q))
        for v in node.values():
            _walk(v, d, q, acc)
    elif isinstance(node, list):
        for v in node:
            _walk(v, done, quantum, acc)


def _live_run_pairs(doc, acc):
    """testing/live-run.js: `counters` and `frontier` are separate arrays ALIGNED INDEX
    FOR INDEX, both taken from one run record, so index is the pairing and the only one."""
    cn, fr = doc.get('counters'), doc.get('frontier')
    if not (isinstance(cn, list) and isinstance(fr, list) and len(cn) == len(fr)):
        return False
    hit = False
    q = doc.get('quantum')
    for i, f in enumerate(fr):
        if isinstance(f, dict) and TOK in f:
            acc.append(((cn[i] or {}).get('unitsDone'), f, q))
            hit = True
    return hit


def read(path):
    """-> (kind, [(unitsDone, census, quantum), ...]) in emission order"""
    try:
        txt = open(path, errors='replace').read()
    except Exception:
        return None, []
    if TOK not in txt:
        return None, []
    try:
        doc = json.loads(txt)
    except Exception:
        doc = None
    if doc is not None:
        acc = []
        if isinstance(doc, dict) and _live_run_pairs(doc, acc):
            return 'live-run', acc
        _walk(doc, None, None, acc)
        if acc:
            return 'json', acc
    acc = []
    for line in txt.splitlines():
        line = line.strip()
        if line.startswith('{'):
            try:
                _walk(json.loads(line), None, None, acc)
            except Exception:
                pass
    if acc:
        return 'jsonl', acc
    acc = []
    i = 0
    while True:
        b = txt.find('{', i)
        if b < 0:
            break
        o = _brace_obj(txt, b)
        if o is None:
            break
        i = b + (len(o) if TOK in o else 1)
        if TOK not in o:
            continue
        try:
            _walk(json.loads(o), None, None, acc)
        except Exception:
            pass
    return ('log' if acc else None), acc


def _slice_ms(q):
    return (q or {}).get('sliceMs', 12)


def summary(paths):
    files = []
    for root in paths:
        walk = [(root,)] if os.path.isfile(root) else []
        if os.path.isdir(root):
            for dp, _dn, fn in os.walk(root):
                walk += [(os.path.join(dp, f),) for f in fn]
        for (p,) in walk:
            try:
                if os.path.getsize(p) > 200_000_000:
                    continue
                if TOK.encode() not in open(p, 'rb').read():
                    continue
            except Exception:
                continue
            files.append((os.path.getmtime(p), p))
    files.sort()  # OLDEST FIRST: a log corpus is biased toward the present, so a claim
                  # over it read newest-first means "not lately" and is heard as "not ever"
    print('# %d artifact(s) carry %s   OLDEST FIRST   (TERMINAL census of each)'
          % (len(files), TOK))
    print('# %-11s %-8s %4s %8s %7s %7s %8s %6s %5s %5s %7s %5s %4s  sum  path'
          % ('mtimeUTC', 'kind', 'n', 'steps', 'ovr', 'ovr%', 'mid', 'mid%',
             'park', 'ckpt', 'done', 'fin', 'cc'))
    for mt, p in files:
        kind, rs = read(p)
        if not rs:
            continue
        done, c, _q = rs[-1]
        st, ov = c.get('steps'), c.get('sliceOverruns')
        mid, pk, ck = c.get('unitMidProgram'), c.get('unitParked'), c.get('unitCheckpointOwed')
        cc = c.get('classicCompiles')          # absent is NOT zero: older artifact
        if done is not None and None not in (st, mid, pk, ck):
            s = mid + pk + ck + done
            chk = 'OK' if s == st else 'BAD %d v %d' % (s, st)
        else:
            chk = 'n/a'
        ts = datetime.datetime.utcfromtimestamp(mt).strftime('%m-%d %H:%M')
        pct = (lambda n: ('%6.2f' % (100.0 * n / st)) if (n is not None and st) else '     -')
        print('  %-11s %-8s %4d %8s %7s %s %8s %s %5s %5s %7s %5s %4s  %-9s %s'
              % (ts, kind, len(rs), st, ov, pct(ov), mid, pct(mid), pk, ck,
                 done, c.get('finished'), '-' if cc is None else cc, chk, p))


def hist(paths):
    for p in paths:
        kind, rs = read(p)
        if not rs:
            print('%s: NO CENSUS' % p)
            continue
        done, c, q = rs[-1]
        print('### %s   kind=%s  censuses=%d  TERMINAL ONLY' % (p, kind, len(rs)))
        if q:
            print('    quantum: measure=%s isCpu=%s sliceMs=%s'
                  % (str(q.get('measure'))[:44], q.get('isCpu'), q.get('sliceMs')))
        print('    steps=%s sliceUs=%s sliceOverruns=%s unitsDone=%s mid=%s park=%s ckpt=%s'
              % (c.get('steps'), c.get('sliceUs'), c.get('sliceOverruns'), done,
                 c.get('unitMidProgram'), c.get('unitParked'), c.get('unitCheckpointOwed')))
        print('    finished=%s forks=%s rootPrograms=%s deepest=%s completed=%s deepestLeft=%s '
              'progStarts=%s classicCompiles=%s classicCompileOverruns=%s'
              % (c.get('finished'), c.get('forks'), c.get('rootPrograms'), c.get('deepest'),
                 c.get('completed'), c.get('deepestLeft'), c.get('progStarts'),
                 c.get('classicCompiles', 'ABSENT'), c.get('classicCompileOverruns', 'ABSENT')))
        runs = c.get('stepUnitRuns') or {}
        over = c.get('stepUnitOverruns') or {}
        if not runs:
            print('    stepUnitRuns ABSENT from this artifact — the driver that wrote it did not read it\n')
            continue
        print('    %-38s %9s %8s %11s' % ('arm', 'runs', 'over', 'over/runs%'))
        for k, r in runs.items():
            o = over.get(k, 0)
            # an arm's ABSENCE from the OVERRUN table is evidence (solver/engine.h), so a
            # row with runs is printed however its overrun count reads; a row with neither
            # is the arm being structurally empty and is dropped.
            if not r and not o:
                continue
            print('    %-38s %9d %8d %11s'
                  % (k, r, o, ('%.2f' % (100.0 * o / r)) if r else '-'))
        sr, so = sum(runs.values()), sum(over.values())
        print('    %-38s %9d %8d' % ('SUM', sr, so))
        print('    sum(runs)==steps           : %s (%s vs %s)' % (sr == c.get('steps'), sr, c.get('steps')))
        if over:
            print('    sum(over)==sliceOverruns   : %s (%s vs %s)'
                  % (so == c.get('sliceOverruns'), so, c.get('sliceOverruns')))
        else:
            print('    stepUnitOverruns ABSENT — solver/engine.h: the runs half alone is not the reading')
        if done is not None:
            s4 = (c.get('unitMidProgram', 0) + c.get('unitParked', 0)
                  + c.get('unitCheckpointOwed', 0) + done)
            print('    4-arm sum==steps           : %s (%s vs %s)' % (s4 == c.get('steps'), s4, c.get('steps')))
        else:
            print('    4-arm sum                  : n/a — no unitsDone in the SAME composed object')
        print()


def _order(p):
    m = re.search(r'[-_]s(\d+)\b', os.path.basename(p))
    return (0, int(m.group(1))) if m else (1, os.path.getmtime(p))


def marginal(paths):
    """consecutive censuses of ONE run: one artifact holding a series, or several
    artifacts each holding one census (an s1..sN poll)."""
    series = []
    if len(paths) == 1:
        _k, rs = read(paths[0])
        series = [(paths[0], d, c, q) for (d, c, q) in rs]
    else:
        for p in sorted(paths, key=_order):
            _k, rs = read(p)
            if rs:
                d, c, q = rs[-1]
                series.append((p, d, c, q))
    print('%-30s %7s %9s %5s %12s %8s  arms gained' %
          ('interval', 'dSteps', 'dSliceS', 'dOvr', 'FLOOR_s/ovr', 'xSlice'))
    for i in range(1, len(series)):
        pp, _pd, pc, _pq = series[i - 1]
        np_, _nd, nc, nq = series[i]
        ms = _slice_ms(nq)
        ds = nc.get('steps', 0) - pc.get('steps', 0)
        do = nc.get('sliceOverruns', 0) - pc.get('sliceOverruns', 0)
        du = nc.get('sliceUs', 0) - pc.get('sliceUs', 0)
        if ds < 0 or do < 0 or du < 0:
            print('  %-28s SERIES FALLS — these are lifetime counts and cannot; two runs, not one'
                  % (os.path.basename(pp) + '->' + os.path.basename(np_)))
            continue
        a = nc.get('stepUnitOverruns') or {}
        b = pc.get('stepUnitOverruns') or {}
        gained = {k: a[k] - b.get(k, 0) for k in a if a[k] - b.get(k, 0)}
        label = '%s->%s' % (os.path.basename(pp)[:13], os.path.basename(np_)[:14])
        if do > 0:
            fl = (du - (ds - do) * ms * 1000) / do / 1e6
            print('  %-28s %7d %9.1f %5d %12.3f %8.0f  %s'
                  % (label, ds, du / 1e6, do, fl, fl * 1000.0 / ms, gained or '(no arm table)'))
        else:
            print('  %-28s %7d %9.1f %5d %12s %8s  (no new overrun)'
                  % (label, ds, du / 1e6, do, '-', '-'))


if __name__ == '__main__':
    a = sys.argv[1:]
    if a and a[0] == '--hist':
        hist(a[1:])
    elif a and a[0] == '--marginal':
        marginal(a[1:])
    elif a:
        summary(a)
    else:
        print(__doc__)
