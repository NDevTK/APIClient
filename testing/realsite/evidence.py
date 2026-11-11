"""One page's first-cause evidence, from a diff.mjs --json file and the engine artifacts it names.
    python3 -I testing/realsite/evidence.py <diff.json>
Printed: how the run ended; the wire mode and the sends a replay's recording lacked; the first requests
Chromium always makes that the engine never sent, with each one's method, kind and engine record; per
instance the program rows reached, the reply door, the clock licence, page errors split into standing and
exploration-produced, the records by provenance and door, the sales by outcome, the engine gaps the instance
crossed (`_instanceGaps`) and the oldest observed-class member's place; the zone's egress decisions by how each
was answered; and per instance the observed world over the run's census samples. `picks` there is the pick
count of the best-weighted observed member at that sample, which is one member's history only while the
observed world holds few members. A replay's recording answers a send by method, URL and body, so a send it
lacks is a miss whatever the engine did; misses are split into GET and the rest."""
import gzip, json, os, sys
from collections import Counter

def lines(path):
    return [json.loads(l) for l in open(path) if l.strip()] if os.path.exists(path) else []

def who(m):
    return 'service-worker' if m['serviceWorker'] else 'subframe' if m['mainFrame'] is False else 'main'

def main(diff_path):
    dj = json.load(open(diff_path))
    wire = dj['wireFile']
    base = wire[:-len('.wire.jsonl')]
    logs = os.path.dirname(wire)
    print('##', os.path.basename(base))
    # A live pass leaves its row in <store>/logs/record.jsonl, a replay in <out>/runs.jsonl.
    page = os.path.basename(base).split('.')[0]
    runs = [r for r in lines(os.path.join(logs, 'record.jsonl')) if r['page'] == page and base.endswith(str(r['run']))]
    runs += [r for r in lines(os.path.join(os.path.dirname(logs), 'runs.jsonl')) if r['page'] == page]
    for r in runs[-1:]:
        print('run: ended', r['endedBy'], 'work', r['stepWorkReached'], 'instances', r['instances'], 'sends', r['wireSends'],
              'memPeakMiB', r['memPeakMiB'], 'wallS', int(r['wallS']), 'load1', r['load1'], 'abort', (r['abort'] or '')[:330])
    w = lines(wire)
    misses = [x for x in w if x['wire'] == 'replay' and not x['hit']]
    print('wire mode', sorted(set(x['wire'] for x in w)), 'sends', len(w), 'sends the recording lacked', len(misses),
          sorted(set(x['method'] + ' ' + x['url'].split('?')[0] for x in misses))[:6])
    for name, ms in (('GET', [x for x in misses if x['method'] == 'GET']), ('not GET', [x for x in misses if x['method'] != 'GET'])):
        ids = Counter(x['method'] + ' ' + x['url'].split('?')[0] for x in ms)
        print('  recording lacked, %s: %d sends, %d addresses' % (name, len(ms), len(ids)), ids.most_common(8))
    eg = [json.loads(l.split('\t', 1)[1]) for l in open(base + '.egress.tsv')] if os.path.exists(base + '.egress.tsv') else []
    print('egress decisions', len(eg), dict(Counter((e['method'], e['how'], e['refusal']) for e in eg)))
    print('chromium: counted', dj['counted'], 'of', dj['loads'], 'loads; stable identities', len(dj['stable']), 'varying',
          len(dj['varying']), '| engine never sent', len(dj['missing']), '| engine sends no load made', len(dj['extra']))
    for m in dj['missing'][:4]:
        ini = m['initiator']
        recs = sorted(set('%s/%s' % (x['provenance'], x['door']) for x in m['records']))
        print('  missing #%d at %sms %s %s %s | by %s | record %s' % (
            m['order'], m['atMs'], m['resourceType'], who(m), m['key'][:120],
            (ini['type'] + ' ' + str(ini['at'])[-70:]) if ini else None, recs or 'none'))
    print('  missing by kind:', dict(Counter((m['resourceType'], who(m)) for m in dj['missing'])))
    print('  missing that are not GET:', sum(1 for m in dj['missing'] if not m['key'].startswith('GET ')))
    extras = Counter((e['key'].split(' ')[1].split('/')[2],
                      ','.join(sorted(set('%s/%s' % (x['provenance'], x['door']) for x in e['records']))) or 'no record')
                     for e in dj['extra'])
    print('  engine-only sends by origin and record:', dict(extras))
    rf = dj['resultsFile']
    docs = json.load(gzip.open(rf) if rf.endswith('.gz') else open(rf))
    for d in docs:
        c, q = d['_cold'], d['_wfq']
        print(' instance', d['_novelty']['scope'], 'stepWork', c['stepWork'], 'flows made', d['_flows'], 'members', q['members'],
              'observed', q['observedMembers'], 'on an assumption', q['assumedMembers'], '| root programs', c['rootPrograms'],
              'deepest started', c['deepest'], 'deepest completed', c['completed'],
              'cursors', {k: v for k, v in c['programCursors'].items() if v}, '| sold', c['sold'], 'resumed', c['resumed'])
        print('   replies asked/answered/declined/dropped/outstanding', c['replyAsked'], c['replyAnswered'], c['replyDeclined'],
              c['replyDropped'], c['replyOutstanding'], '| fetch() called', c['epFetchAskCalledLife'], 'XHR send() called',
              c['epXhrAskCalledLife'], '| clock advance asked/declined', c['clockAdvanceAskedLife'], c['clockAdvanceDeclinedLife'],
              'timer units run', sum(v for k, v in c['stepUnitRuns'].items() if 'timer' in k), '| orphans driven', d['_orphansDriven'])
        standing = [e for e in d['pageErrors'] if e not in d['pageErrorsExplored']]
        print('   page errors', len(d['pageErrors']), 'of which exploration-produced', len(d['pageErrors']) - len(standing))
        for e in standing[:4]: print('     standing:', e[:260])
        for e in [e for e in d['pageErrors'] if e in d['pageErrorsExplored']][:3]: print('     explored:', e[:260])
        print('   standard globals the realm lacks that the page asked for:', [k for k in d['_absent'] if not k.startswith('_')][:25])
        print('   records:', dict(Counter((r['provenance'], r['door'], r['method'], 'driven' if r['driven'] else '') for r in d['fetchCallSites'])))
        print('   replies declined by ground:', {k: v for k, v in c.items() if k.startswith('replyDeclined') and k != 'replyDeclined' and v})
        print('   sales by outcome:', {k: c[k] for k in c if k.startswith('sold')}, '| stubs standing', c.get('cold'),
              'waiting for the park', c.get('coldWaiting'), 'returned', c.get('returned'), 'return bodies', c.get('returnBodies'))
        old, units = q.get('observedOldest'), list(c['stepUnitRuns'])
        print('   oldest observed-class member:', None if not old else dict(old, stepUnit=units[old['stepUnit']]),
              '| observed members', q['observedMembers'], 'left out only for an arm against an environment example',
              q.get('observedEnvMembers'), '| descents reaching the idle rung', c['stepReachedIdleLife'],
              'idle periods run', c['stepUnitRuns']['start-or-run-an-idle-period'])
        gaps = d.get('_instanceGaps')
        print('   engine gaps crossed:', None if gaps is None else len(gaps))
        for g in gaps or []:
            print('     gap %s:%s reached %s%s: %s' % (g['siteFile'], g['siteLine'], g['reached'],
                                                    ' (composed)' if g['composed'] else '', g['message'][:300]))
    cold, rows = {}, {}
    for l in gzip.open(base + '.census.tsv.gz', 'rt'):
        tag, marker, payload = l.rstrip('\n').split('\t', 2)
        j = json.loads(payload)
        if marker == '@COLD':
            cold[tag] = j
        elif marker == '@WFQ' and tag in cold:
            o = (j.get('observedTerms') or {}).get('observed')
            rows.setdefault(tag, []).append((cold[tag]['stepWork'], j['members'], j['observedMembers'], j['observedWGap'],
                                             o['picks'] if o else None, o['hostOwed'] if o else None,
                                             cold[tag]['sold'], cold[tag]['resumed'], cold[tag]['completed']))
    for tag, rs in rows.items():
        print(' observed world,', tag, '- (stepWork, members, observed members, observedWGap, best observed member picks,'
              ' its hostOwed, sold, resumed, deepest completed):')
        turn = [i for i in range(1, len(rs)) if (rs[i][2] == 0) != (rs[i - 1][2] == 0)]
        keep = sorted(set([0, 1, 2, len(rs) // 4, len(rs) // 2, 3 * len(rs) // 4, len(rs) - 2, len(rs) - 1] + turn + [i - 1 for i in turn]))
        for i in keep:
            if 0 <= i < len(rs): print('    ', rs[i])
        picks = [r[4] for r in rs if r[4] is not None]
        if picks: print('     picks over all', len(rs), 'samples: max', max(picks), 'last', picks[-1])

if __name__ == '__main__':
    if len(sys.argv) != 2:
        sys.exit('usage: python3 -I testing/realsite/evidence.py <diff.json>')
    main(sys.argv[1])
