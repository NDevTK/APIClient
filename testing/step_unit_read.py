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

WHAT THE STEP ROWS CANNOT SAY, AND THE ROWS THAT SAY IT.  `stepUnitRuns` names WHICH ARM a
step took and `sliceUs` says what the turns cost.  Neither can separate two costs that live
INSIDE ONE ARM, and that is the shape a real page presents: a page whose time is spent in
the interpreter and a page whose time is spent in engine C reached FROM the interpreter both
report `start-a-classic-program`.  So a per-arm time accumulator — the instrument a reader
reaches for next — would attribute the same microseconds to the same arm under both readings
and settle neither.  What settles it is a SECOND SERIES paired with the first: the COW
host-record capture census (`_swap`'s `cowStateAsks.hostRec`, `cowStateMade.hostRec` and the
per-site `cowHostRecAsksBySite`) and the run's own `_sourceReads`, differenced over the SAME
intervals.  A cost that is constant PER ASK while the cost PER STEP varies by orders of
magnitude across those same intervals is a per-ask cost, and that inference needs no model of
what the ask does and no number written down here.  Those rows are read from the NEAREST
ENCLOSING object of the census, which is the same pairing rule this file already applies to
`unitsDone` and the only one it makes.

THE RATIOS PRINTED ARE BOUNDS AND THE COLUMN NAMES SAY SO.  `US/ASK` charges a whole interval
to the asks inside it, so it is an UPPER bound on what one ask costs; its CONSTANCY across
intervals of different `dSteps` is the reading, never its value at any one of them.

ABSENT IS NOT ZERO AND IS PRINTED AS ABSENT.  testing/live-run.js carries `forkAt` and part
of `cold` and deliberately carries neither `heap` nor `swap` ("a driver that took everything
would be a second copy of the popup"), so a `live-run` document cannot be asked this question
at all and this file says so rather than rendering a 0.  NEXT DIFF: carry `cowStateAsks` and
`cowHostRecAsksBySite` — those two rows and not `swap` whole, which keeps that driver's own
argument intact.  HOW ITS ABSENCE SHOWS: the capture block below reads ABSENT for every
artifact that driver writes, while a JSONL probe of the same page answers.
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


def _walk(node, done, quantum, side, acc):
    """collect (unitsDone, census, quantum, side) in document order, each of the three
    outer members taken from the NEAREST ENCLOSING object so every one of them comes from
    ONE composed document.  `side` is the capture census and the source-read total, which
    are SIBLINGS of the step census (`_swap` and `_sourceReads` beside `_cold`) rather than
    rows of it — the same legitimacy argument as `unitsDone`, and the same rule."""
    if isinstance(node, dict):
        d = node.get('unitsDone', node.get('_unitsDone', done))
        q = node.get('quantum', node.get('_quantum', quantum))
        sw = node.get('_swap', node.get('swap'))
        sr = node.get('_sourceReads', node.get('sourceReads'))
        # `bool` IS an `int` in Python and a row that is `true` is not a count, so the
        # narrowing is explicit rather than left to `isinstance` — the same reason
        # censusHistRows refuses a non-numeric row instead of summing it.
        sr_ok = isinstance(sr, int) and not isinstance(sr, bool)
        sd = side
        if isinstance(sw, dict) or sr_ok:
            sd = dict(side or {})
            if isinstance(sw, dict):
                sd['swap'] = sw
            if sr_ok:
                sd['srcReads'] = sr
        if TOK in node:
            acc.append((d, node, q, sd))
        for v in node.values():
            _walk(v, d, q, sd, acc)
    elif isinstance(node, list):
        for v in node:
            _walk(v, done, quantum, side, acc)


def _capture(side):
    """-> (hostRecAsks, hostRecMade, sitesDict, sourceReads), each None where the artifact
    does not carry it.  The site keys are the engine's own `file:line` strings and are never
    listed here: a second list of capture sites would drift exactly as a second list of
    solver/step_unit.h's arms would."""
    sw = (side or {}).get('swap') or {}
    asks = (sw.get('cowStateAsks') or {}).get('hostRec')
    made = (sw.get('cowStateMade') or {}).get('hostRec')
    sites = sw.get('cowHostRecAsksBySite')
    if not isinstance(sites, dict):
        sites = None
    return asks, made, sites, (side or {}).get('srcReads')


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
            # `side` is None and never {}: that driver carries no `swap` and no
            # `_sourceReads` at all, which is an ABSENT capture census and not an
            # empty one, and the two are printed differently.
            acc.append(((cn[i] or {}).get('unitsDone'), f, q, None))
            hit = True
    return hit


def read(path):
    """-> (kind, [(unitsDone, census, quantum, side), ...]) in emission order"""
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
        _walk(doc, None, None, None, acc)
        if acc:
            return 'json', acc
    acc = []
    for line in txt.splitlines():
        line = line.strip()
        if line.startswith('{'):
            try:
                _walk(json.loads(line), None, None, None, acc)
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
            _walk(json.loads(o), None, None, None, acc)
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
        done, c, _q, _sd = rs[-1]
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
        done, c, q, side = rs[-1]
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
        _capture_block(c, side)
        print()


def _capture_block(c, side):
    """WHAT THE STEPS WERE DOING, which no row of the step census can say — see the module
    docstring.  Printed under the arm table because it is read WITH it and means little
    alone: the arm names where a step ended and these name the work inside it."""
    asks, made, sites, sr = _capture(side)
    if asks is None and sites is None and sr is None:
        print('    capture census             : ABSENT — this artifact carries no `_swap` and no '
              '`_sourceReads`, so it cannot be asked what its steps were DOING. That is an absent '
              'measurement and not a zero; see the module docstring for which driver this is.')
        return
    su = c.get('sliceUs')
    print('    sourceReads=%s  hostRec asks=%s made=%s' % (sr, asks, made))
    if su and asks:
        print('    sliceUs/asks = %.3f us per ask   UPPER BOUND: it charges every microsecond of the '
              'run to these asks.' % (su / float(asks)))
        print('    Its CONSTANCY across --marginal intervals of different dSteps is the reading; its '
              'value at any one interval is not.')
    if sites:
        print('    %-58s %14s %7s' % ('capture site', 'asks', 'share'))
        for k, v in sorted(sites.items(), key=lambda x: -x[1]):
            print('    %-58s %14d %6.2f%%'
                  % (k, v, (100.0 * v / asks) if asks else float('nan')))
        tot = sum(sites.values())
        print('    %-58s %14d %6.2f%%' % ('SUM (rows are a PARTITION of the asks)', tot,
                                          (100.0 * tot / asks) if asks else float('nan')))
        if asks is not None and tot != asks:
            print('    ROWS DO NOT SUM TO THE ASKS (%d vs %d) — solver/cow.c raises a site count in the '
                  'same breath as the kind and DCHECKs the identity, so a difference here is a row lost '
                  'between that assert and this document.' % (tot, asks))


def _order(p):
    m = re.search(r'[-_]s(\d+)\b', os.path.basename(p))
    return (0, int(m.group(1))) if m else (1, os.path.getmtime(p))


def marginal(paths):
    """consecutive censuses of ONE run: one artifact holding a series, or several
    artifacts each holding one census (an s1..sN poll)."""
    series = []
    if len(paths) == 1:
        _k, rs = read(paths[0])
        series = [(paths[0], d, c, q, sd) for (d, c, q, sd) in rs]
    else:
        for p in sorted(paths, key=_order):
            _k, rs = read(p)
            if rs:
                d, c, q, sd = rs[-1]
                series.append((p, d, c, q, sd))
    q0 = next((x[3] for x in series if x[3]), None)
    print('# quantum: measure=%s isCpu=%s sliceMs=%s   (the FLOOR column is in this measure; on a WALL '
          'host descheduling can only push it UP)'
          % (str((q0 or {}).get('measure'))[:40], (q0 or {}).get('isCpu'), _slice_ms(q0)))
    print('%-30s %7s %9s %5s %12s %8s %10s %12s %9s  arms gained' %
          ('interval', 'dSteps', 'dSliceS', 'dOvr', 'FLOOR_s/ovr', 'xSlice', 'dSrcReads', 'dCaptAsks',
           'US/ASK*'))
    for i in range(1, len(series)):
        pp, _pd, pc, _pq, psd = series[i - 1]
        np_, _nd, nc, nq, nsd = series[i]
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
        # THE SECOND SERIES, DIFFERENCED OVER THE SAME INTERVAL. Absent on either side is
        # printed as `-` and never as 0: an artifact that does not carry these rows and a run
        # that made no ask are different facts and this file will not average them.
        pa, _pm, _ps, psr = _capture(psd)
        na, _nm, _ns, nsr = _capture(nsd)
        num = lambda x: isinstance(x, int) and not isinstance(x, bool)
        dca = (na - pa) if (num(pa) and num(na)) else None
        dsr = (nsr - psr) if (num(psr) and num(nsr)) else None
        if dca is not None and dca < 0:
            dca = 'FALLS'
        if dsr is not None and dsr < 0:
            dsr = 'FALLS'
        # UPPER BOUND: the whole interval charged to the asks inside it.
        upa = ('%.3f' % (du / dca)) if num(dca) and dca > 0 else '-'
        dca = '-' if dca is None else dca
        dsr = '-' if dsr is None else dsr
        if do > 0:
            fl = (du - (ds - do) * ms * 1000) / do / 1e6
            print('  %-28s %7d %9.1f %5d %12.3f %8.0f %10s %12s %9s  %s'
                  % (label, ds, du / 1e6, do, fl, fl * 1000.0 / ms, dsr, dca, upa,
                     gained or '(no arm table)'))
        else:
            print('  %-28s %7d %9.1f %5d %12s %8s %10s %12s %9s  (no new overrun)'
                  % (label, ds, du / 1e6, do, '-', '-', dsr, dca, upa))


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
