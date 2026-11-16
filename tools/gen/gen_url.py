#!/usr/bin/env python3
"""Generates the URL special-scheme lookup and default-port table, and the rules of the Public Suffix List."""

import os

from tools.gen.gen_common import ROOT, banner, table, write
from tools.gen.name_table import NameTable

# url/#special-scheme. These names and ports come from the spec's prose, not from Web IDL data.
SCHEMES = {"file": -1, "ftp": 21, "http": 80, "https": 443, "ws": 80, "wss": 443}

PUBLIC_SUFFIXES = os.path.join(ROOT, "third_party", "publicsuffix", "public_suffix_list.dat")

# The kinds of rule of the list (publicsuffix.org/list/), which one name can have several of.
RULE_EXACT, RULE_WILDCARD, RULE_EXCEPTION = 1, 2, 4


def ascii_name(name):
    """Returns `name` with each label that is not ASCII as its A-label, as a URL's host holds it."""
    labels = []
    for label in name.split("."):
        if label.isascii():
            labels.append(label.lower())
        else:
            labels.append("xn--" + label.encode("punycode").decode("ascii"))
    return ".".join(labels)


def public_suffix_rules():
    """Returns the list's rules as (name, kinds), sorted by name: a wildcard rule `*.x` as `x`, and an exception rule
    `!x` as `x`."""
    rules = {}
    with open(PUBLIC_SUFFIXES, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("//"):
                continue
            rule = line.split()[0]
            kind = RULE_EXACT
            if rule.startswith("!"):
                rule, kind = rule[1:], RULE_EXCEPTION
            elif rule.startswith("*."):
                rule, kind = rule[2:], RULE_WILDCARD
            name = ascii_name(rule)
            rules[name] = rules.get(name, 0) | kind
    return sorted(rules.items(), key=lambda item: item[0].encode("ascii"))


def write_public_suffixes(source):
    rules = public_suffix_rules()
    generated = banner(__file__, "the Public Suffix List", "https://publicsuffix.org/list/public_suffix_list.dat")
    header = [generated, '\n#pragma once\n\n#include "pr/base.h"\n',
              '\nPR_ASSUME_NONNULL_BEGIN\n\nnamespace pr_impl {\n',
              '\n// The kinds of rule that the Public Suffix List has for a name. A wildcard rule `*.name` makes every name '
              'of one\n// more label a public suffix, and an exception rule `!name` makes the name no public suffix.\n',
              'enum {\n    UrlSuffixRuleFlag_Exact = 1u << 0,\n    UrlSuffixRuleFlag_Wildcard = 1u << 1,\n'
              '    UrlSuffixRuleFlag_Exception = 1u << 2,\n};\ntypedef uint8_t UrlSuffixRuleFlags;\n',
              '\n// Returns the rules that the list has for `name`, a domain in lowercase ASCII, or none.\n',
              'UrlSuffixRuleFlags urlSuffixRules(PrStringView name);\n',
              '\n} // namespace pr_impl\n\nPR_ASSUME_NONNULL_END\n']
    write(os.path.join(source, "url_public_suffix.h"), header)
    text = "".join(name for name, _ in rules)
    offsets = []
    at = 0
    for name, kinds in rules:
        offsets.append("{%d, %d, %d}" % (at, len(name), kinds))
        at += len(name)
    lines = []
    for start in range(0, len(text), 100):
        lines.append('    "%s"\n' % text[start:start + 100])
    body = [generated, '\n#include "url/src/url_public_suffix.h"\n',
            '\n#include "pr/count_of.h"\n#include "pr/search.h"\n#include "pr/string_ops.h"\n',
            '\nnamespace pr_impl {\nnamespace {\n',
            '\n// The names of the rules, one after another, in the order of kRules.\n',
            'constexpr char kNames[] =\n'] + lines + [';\n',
            '\n// A name of kNames and the rules that the list has for it.\n',
            'struct SuffixRule {\n    uint32_t offset;\n    uint8_t size;\n    UrlSuffixRuleFlags kinds;\n};\n',
            '\n// The rules, sorted by name.\n',
            table('constexpr SuffixRule kRules[]', offsets, per_line=6),
            '\n// Returns the name of the rule at `index`.\n',
            'PrStringView ruleName(uint32_t index) {\n',
            '    return PrStringView{kNames + kRules[index].offset, kRules[index].size};\n}\n',
            '\n} // namespace\n',
            '\nUrlSuffixRuleFlags urlSuffixRules(PrStringView name) {\n',
            '    uint32_t count = uint32_t(countOf(kRules));\n',
            '    uint32_t index = partitionPoint(0, count, [&](uint32_t i) { return prStringCompare(ruleName(i), name) < 0; });\n',
            '    return index < count && ruleName(index) == name ? kRules[index].kinds : 0;\n}\n',
            '\n} // namespace pr_impl\n']
    write(os.path.join(source, "url_public_suffix.cpp"), body)


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
    write_public_suffixes(source)


if __name__ == '__main__':
    main()
