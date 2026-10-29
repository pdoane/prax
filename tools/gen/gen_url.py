#!/usr/bin/env python3
"""Generates the URL special-scheme lookup and default-port table."""

import os

from tools.gen.gen_common import ROOT, banner, table, write
from tools.gen.name_table import NameTable

# url/#special-scheme. These names and ports come from the spec's prose, not from Web IDL data.
SCHEMES = {"file": -1, "ftp": 21, "http": 80, "https": 443, "ws": 80, "wss": 443}


def main():
    names = [""] + list(SCHEMES)
    lookup = NameTable("kUrlSchemeName", names, lower=False)
    source = os.path.join(ROOT, "lib/url/src")
    generated = banner(__file__, "URL Standard special schemes",
                       "https://url.spec.whatwg.org/#special-scheme")
    header = [generated, '\n#pragma once\n\n#include "pr/base.h"\n',
              '\nPR_ASSUME_NONNULL_BEGIN\n\nnamespace pr_impl {\n',
              '\n// The special schemes (url/#special-scheme), and UrlScheme_None for any other.\nenum {\n',
              '    UrlScheme_None,\n']
    header += ['    UrlScheme_%s,\n' % name.capitalize() for name in SCHEMES]
    header += ['    UrlScheme_Count,\n};\ntypedef uint8_t UrlScheme;\n',
               '\n// Each scheme\'s default port, or -1 for none.\n',
               'extern const int32_t kUrlSchemePort[UrlScheme_Count];\n',
               '\n// The special scheme that `name` names, or UrlScheme_None. `name` is in lowercase ASCII.\n',
               'UrlScheme urlScheme(PrStringView name);\n',
               '\n} // namespace pr_impl\n\nPR_ASSUME_NONNULL_END\n']
    write(os.path.join(source, "url_scheme.h"), header)
    body = [generated, '\n#include "url/src/url_scheme.h"\n',
            '\n#include "pr/name_table.h"\n\nnamespace pr_impl {\nnamespace {\n',
            lookup.tables(), '\n} // namespace\n',
            table('extern const int32_t kUrlSchemePort[UrlScheme_Count]',
                  ['-1'] + [str(port) for port in SCHEMES.values()]),
            '\nUrlScheme urlScheme(PrStringView name) {\n', lookup.lookup("name", "index"),
            '    return %s ? UrlScheme(index) : UrlScheme_None;\n}\n'
            % lookup.confirm("name", lookup.name("index")), '\n} // namespace pr_impl\n']
    write(os.path.join(source, "url_scheme.cpp"), body)


if __name__ == '__main__':
    main()
