#!/usr/bin/env python3
"""Compares the native URL parser with WPT's urltestdata.json.

The run fails on a disagreement. Origin and setters are outside this parser suite. All exposed components and
serialization stability are checked.
"""

import json
import os
import sys

from tools.suite import corpus


def utf8(text):
    # IDL USVString conversion replaces isolated surrogates before URL parsing.
    return ''.join('\ufffd' if 0xd800 <= ord(c) <= 0xdfff else c for c in text).encode('utf-8')


def main():
    args = corpus.arguments(corpus.binary("url_parsing"),
                            os.path.join(corpus.ROOT, 'third_party', 'wpt', 'url', 'resources',
                                         'urltestdata.json')).parse_args()
    if not os.path.isfile(args.tests):
        sys.exit('URL corpus missing; run ./dev.sh fetch wpt')
    with open(args.tests, encoding='utf-8') as source:
        data = json.load(source)
    suite = corpus.Data('url-parsing')
    keys = {'urltestdata:%d' % i for i, case in enumerate(data) if isinstance(case, dict)}
    cases = [(i, case) for i, case in enumerate(data)
             if isinstance(case, dict) and suite.runs('urltestdata:%d' % i, args.match)]
    if not cases:
        sys.exit('no URL cases selected')
    if args.list:
        print('%d URL cases' % len(cases))
        return 0
    corpus.require(args.binary, 'url-parsing')
    frames = []
    for _, case in cases:
        base = utf8(case.get('base') or '')
        payload = base + utf8(case['input'])
        field = str(len(base)) if case.get('base') is not None else '-'
        frames.append(('%d %s\n' % (len(payload), field)).encode() + payload)
    results = corpus.answers([args.binary], b''.join(frames), len(cases))
    passed = recorded = 0
    failures = []
    outcomes = {}
    components = ('href', 'protocol', 'username', 'password', 'hostname', 'port', 'pathname',
                  'search', 'hash')
    for (index, case), result in zip(cases, results):
        key = 'urltestdata:%d' % index
        if case.get('failure'):
            agrees = result['status'] == 'Syntax'
        else:
            agrees = (result['status'] == '' and all(result.get(k) == case[k] for k in components)
                      and result.get('reparsed') == case['href'])
        reason = suite.recorded(key)
        outcomes[key] = 1 if agrees else 0
        if agrees:
            passed += 1
            if reason is not None:
                failures.append(index)
                print('%s agrees now; drop its erratum' % key)
        elif reason is not None:
            recorded += 1
            if args.verbose:
                print('%s\n    %s' % (key, reason))
        else:
            failures.append(index)
            print('%s input=%r base=%r' % (key, case['input'], case.get('base')))
            print('  expected', json.dumps(case, ensure_ascii=True))
            print('  got     ', json.dumps(result, ensure_ascii=True))
    print('%d/%d agree; %d recorded; %d disagree' % (passed, len(cases), recorded, len(failures)))
    corpus.outcomes(args, outcomes)
    if corpus.writes_scoreboard(args):
        suite.write_scoreboard([('urltestdata', passed + recorded, len(cases))])
    stale = [] if args.match else suite.stale(keys)
    for kind, key in stale:
        print('%s names no case (%s)' % (key, kind))
    return int(bool(failures or stale))

if __name__ == '__main__':
    sys.exit(main())
