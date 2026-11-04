#!/usr/bin/env python3
"""Code generator for the languages, scripts and regions that lib/locale names (docs/text.md, "Languages and scripts").

Reads these sources, each from its authority and at a pinned version (./dev.sh fetch):

    the IANA Language Subtag Registry and ISO 639-3's code table, as CLDR keeps them
    CLDR's likely subtags, populations, parent locales and each locale's delimiters
    UAX #31's Table 5, the scripts in modern use
    tools/gen/language/opentype.txt, the OpenType language system and script tag registries as this generator last read
        them (--refresh), because Microsoft does not publish them at a versioned URL

and emits:

    lib/locale/include/pr/locale.h          the types and functions, and every script
    lib/locale/include/pr/locale_common.h   every region, and the languages most used
    lib/locale/include/pr/locale_full.h     every language
    lib/locale/src/locale_data.h, .cpp      the tables that lib/locale/src/locale.cpp resolves tags with

--check compares the OpenType languages of each language with HarfBuzz's table, which is not a source, and reports
where they differ.

Usage: python3 -m tools.gen.gen_locale [--refresh] [--check]
"""

import argparse
import collections
import datetime
import html
import os
import re
import urllib.request

from tools.gen import gen_common

ROOT = gen_common.ROOT
CLDR = os.path.join(ROOT, "third_party", "cldr")
CLDR_DATA = os.path.join(CLDR, "tools", "cldr-code", "src", "main", "resources", "org", "unicode", "cldr", "util",
                         "data")
REGISTRY = os.path.join(CLDR_DATA, "language-subtag-registry")
ISO_639_3 = os.path.join(CLDR_DATA, "iso-639-3.tab")
LIKELY = os.path.join(CLDR, "common", "supplemental", "likelySubtags.xml")
SUPPLEMENTAL = os.path.join(CLDR, "common", "supplemental", "supplementalData.xml")
MAIN = os.path.join(CLDR, "common", "main")
UAX31 = os.path.join(ROOT, "third_party", "unicode", "tr31-45.html")
OPENTYPE = os.path.join(ROOT, "tools", "gen", "language", "opentype.txt")
HARFBUZZ_TAGS = os.path.join(ROOT, "third_party", "harfbuzz", "src", "hb-ot-tag-table.hh")

OPENTYPE_LANGUAGES_URL = "https://learn.microsoft.com/en-us/typography/opentype/spec/languagetags"
OPENTYPE_SCRIPTS_URL = "https://learn.microsoft.com/en-us/typography/opentype/spec/scripttags"

# The minimum number of people that CLDR must count as using a language for pr/locale_common.h to name it.
COMMON_POPULATION = 50_000_000

# ISO 15924's special codes, numbered 990 to 999: inherited, emoji, mathematical notation, symbols, unwritten,
# undetermined (Common) and unknown.
SPECIAL_SCRIPTS = {"Zinh", "Zsye", "Zmth", "Zsym", "Zxxx", "Zyyy", "Zzzz"}

# Scripts that are written together, mapped to the combined codes that a language's likely script uses for them
# (ISO 15924's Jpan, Kore, Hans and Hant).
COMBINED = {"Hani": ("Hans", "Hant", "Jpan", "Kore"), "Hira": ("Jpan",), "Kana": ("Jpan",), "Hang": ("Kore",),
            "Bopo": ("Hant",)}


# ----------------------------------------------------------------------------------------------------------------------
# The IANA Language Subtag Registry (RFC 5646 section 3)

def registry_records(path=REGISTRY):
    """Returns the registry's records, each a dict of its fields. A field that may repeat holds a list."""
    with open(path, encoding="utf-8") as f:
        text = f.read()
    records = []
    for block in text.split("\n%%\n")[1:]:
        fields = collections.defaultdict(list)
        key = None
        for line in block.split("\n"):
            if line.startswith("  ") and key is not None:
                fields[key][-1] += " " + line.strip()
            elif ":" in line:
                key, value = line.split(":", 1)
                fields[key].append(value.strip())
        records.append({k: v if k in ("Description", "Comments", "Prefix") else v[0] for k, v in fields.items()})
    return records


def registry_date(path=REGISTRY):
    with open(path, encoding="utf-8") as f:
        return f.readline().split(":", 1)[1].strip()


class Registry:
    """The parts of the registry that the engine names: every language, script and region except the private-use
    ranges, each deprecated subtag that has a preferred value, as an alias of that value, extlangs, and the tags
    registered whole."""

    def __init__(self, records):
        self.date = None
        self.languages = []         # canonical subtags, lowercase
        self.language_alias = {}    # deprecated subtag -> its preferred value
        self.macrolanguage = {}     # subtag -> its macrolanguage
        self.suppress_script = {}   # subtag -> the script that its text is written in
        self.scripts = []           # codes, titlecase
        self.script_name = {}
        self.regions = []           # codes, uppercase
        self.region_alias = {}
        self.extlang = {}           # extlang -> (prefix, preferred value)
        self.whole = {}             # grandfathered or redundant tag, lowercase -> preferred value or None
        self.description = {}
        for r in records:
            kind, subtag = r["Type"], r.get("Subtag")
            if subtag is not None and ".." in subtag:
                continue    # a private-use range
            preferred = r.get("Preferred-Value")
            if kind == "language":
                self.description[subtag] = r["Description"][0]
                if "Macrolanguage" in r:
                    self.macrolanguage[subtag] = r["Macrolanguage"]
                if "Suppress-Script" in r:
                    self.suppress_script[subtag] = r["Suppress-Script"]
                if "Deprecated" in r and preferred is not None:
                    self.language_alias[subtag] = preferred
                else:
                    self.languages.append(subtag)
            elif kind == "script":
                self.scripts.append(subtag)
                self.script_name[subtag] = r["Description"]
            elif kind == "region":
                if "Deprecated" in r and preferred is not None:
                    self.region_alias[subtag] = preferred
                else:
                    self.regions.append(subtag)
            elif kind == "extlang":
                self.extlang[subtag] = (r["Prefix"][0], preferred)
            elif kind in ("grandfathered", "redundant"):
                tag = r["Tag"].lower()
                if kind == "grandfathered" or preferred is not None:
                    self.whole[tag] = preferred
        # Zzzz (Unknown) is first, so that zeroed storage is an unknown script.
        self.scripts.sort(key=lambda code: (code != "Zzzz", code))
        self.languages.sort()
        self.regions.sort()
        self.language_index = {s: i + 1 for i, s in enumerate(self.languages)}
        self.script_index = {s: i for i, s in enumerate(self.scripts)}
        self.region_index = {s: i + 1 for i, s in enumerate(self.regions)}

    def language(self, subtag):
        """Returns the canonical language of a subtag, or None."""
        subtag = subtag.lower()
        subtag = self.language_alias.get(subtag, subtag)
        return subtag if subtag in self.language_index else None

    def region(self, subtag):
        subtag = subtag.upper()
        subtag = self.region_alias.get(subtag, subtag)
        return subtag if subtag in self.region_index else None


def registry():
    r = Registry(registry_records())
    r.date = registry_date()
    return r


# ----------------------------------------------------------------------------------------------------------------------
# ISO 639-3, which names the languages of the OpenType registry

def iso_639_3():
    """Returns the ISO 639-1 code of each ISO 639-3 code that has one."""
    part1 = {}
    with open(ISO_639_3, encoding="utf-8") as f:
        for line in f.readlines()[1:]:
            fields = line.rstrip("\n").split("\t")
            if len(fields) > 3 and fields[3]:
                part1[fields[0]] = fields[3]
    return part1


# ----------------------------------------------------------------------------------------------------------------------
# CLDR

def cldr_locale(text):
    """Returns a CLDR locale id (`zh_Hant_TW`, `und_Hani`) as (language, script or None, region or None)."""
    parts = text.split("_")
    language, script, region = parts[0], None, None
    for part in parts[1:]:
        if len(part) == 4 and part.isalpha():
            script = part
        elif re.fullmatch(r"[A-Z]{2}|[0-9]{3}", part):
            region = part
    return language, script, region


def likely_subtags():
    """Returns CLDR's likely subtags, mapping each locale to the locale that it maximizes to."""
    with open(LIKELY, encoding="utf-8") as f:
        text = f.read()
    return [(cldr_locale(a), cldr_locale(b)) for a, b in re.findall(r'<likelySubtag from="([^"]+)" to="([^"]+)"',
                                                                   text)]


def populations():
    """Returns the number of people that CLDR counts as using each language, summed over every territory, by the
    language's CLDR id."""
    with open(SUPPLEMENTAL, encoding="utf-8") as f:
        text = f.read()
    counts = collections.Counter()
    for territory in re.finditer(r'<territory type="[A-Z0-9]+"[^>]*population="([0-9.Ee+]+)"[^>]*>(.*?)</territory>',
                                 text, re.S):
        people = float(territory.group(1))
        for share in re.finditer(r'<languagePopulation type="([a-zA-Z_]+)"[^>]*populationPercent="([0-9.Ee+-]+)"',
                                 territory.group(2)):
            counts[share.group(1)] += people * float(share.group(2)) / 100
    return counts


def parent_locales():
    """Returns CLDR's explicit parent of each locale that has one, for the data that a locale inherits (UTS #35's
    parentLocales without a component)."""
    with open(SUPPLEMENTAL, encoding="utf-8") as f:
        text = f.read()
    parents = {}
    for block in re.finditer(r"<parentLocales>(.*?)</parentLocales>", text, re.S):
        for parent, children in re.findall(r'<parentLocale parent="([^"]+)"[^>]*locales="([^"]+)"', block.group(1)):
            for child in children.split():
                parents[child] = parent
    return parents


def delimiters():
    """Returns the quotation marks of each CLDR locale that gives any, as a dict of the fields that it sets."""
    out = {}
    for name in sorted(os.listdir(MAIN)):
        if not name.endswith(".xml"):
            continue
        with open(os.path.join(MAIN, name), encoding="utf-8") as f:
            text = f.read()
        block = re.search(r"<delimiters>(.*?)</delimiters>", text, re.S)
        if block is None:
            continue
        marks = {}
        for key in ("quotationStart", "quotationEnd", "alternateQuotationStart", "alternateQuotationEnd"):
            m = re.search(r"<%s>(.*?)</%s>" % (key, key), block.group(1))
            # CLDR writes ↑↑↑ for a value that the locale inherits.
            if m is not None and m.group(1) != "\u2191\u2191\u2191":
                marks[key] = html.unescape(m.group(1))
        out[name[:-4]] = marks
    return out


# ----------------------------------------------------------------------------------------------------------------------
# UAX #31

def recommended_scripts():
    """Returns the scripts in UAX #31's Table 5, the scripts in modern use, except Common and Inherited."""
    with open(UAX31, encoding="utf-8") as f:
        text = f.read()
    at = text.index('name="Table_Recommended_Scripts"')
    body = text[at:text.index("</table>", text.index("<table", at))]
    codes = re.findall(r"\\p\{script=([A-Z][a-z]{3})\}", body)
    return [c for c in codes if c not in ("Zyyy", "Zinh")]


# ----------------------------------------------------------------------------------------------------------------------
# The OpenType registries, as tools/gen/language/opentype.txt holds them

def read_opentype_page(url):
    """Returns the rows of the registry table on the page at `url`, each as a list of the text of its cells."""
    with urllib.request.urlopen(url) as response:
        text = response.read().decode("utf-8")
    rows = []
    for row in re.findall(r"<tr>(.*?)</tr>", text, re.S):
        cells = [html.unescape(re.sub(r"<[^>]+>", "", c)).replace("\xa0", " ").strip()
                 for c in re.findall(r"<td[^>]*>(.*?)</td>", row, re.S)]
        if cells:
            rows.append(cells)
    return rows


def refresh_opentype():
    """Reads the OpenType registries again and writes them to tools/gen/language/opentype.txt."""
    lines = ["# The OpenType language system and script tag registries, as tools/gen/gen_locale.py --refresh read them",
             "# on %s from:" % datetime.date.today().isoformat(),
             "#   %s" % OPENTYPE_LANGUAGES_URL,
             "#   %s" % OPENTYPE_SCRIPTS_URL,
             "# Microsoft publishes them at no versioned URL, so this copy is the version the generator reads. A line",
             "# holds, separated by tabs, a kind, a tag with its trailing spaces dropped, for a language system the",
             "# ISO 639 codes it covers, the name, and `deprecated` where the registry deprecates the tag.",
             ""]
    # A tag is quoted if it ends in a space, and may be marked deprecated.
    def tag_of(cell):
        m = re.search(r"'([A-Za-z0-9 ]{3,4})'", cell) or re.match(r"([A-Za-z0-9]{4})", cell)
        return m.group(1).rstrip(), "deprecated" in cell.lower()
    for name, cell, *_ in read_opentype_page(OPENTYPE_SCRIPTS_URL):
        tag, deprecated = tag_of(cell)
        lines.append("\t".join(["script", tag, name] + (["deprecated"] if deprecated else [])))
    for name, cell, codes, *_ in read_opentype_page(OPENTYPE_LANGUAGES_URL):
        tag, deprecated = tag_of(cell)
        codes = ",".join(c for c in re.split(r"[,\s]+", codes) if re.fullmatch(r"[a-z]{3}", c))
        lines.append("\t".join(["language", tag, codes, name] + (["deprecated"] if deprecated else [])))
    os.makedirs(os.path.dirname(OPENTYPE), exist_ok=True)
    with open(OPENTYPE, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    print("wrote %s" % OPENTYPE)


OpenTypeScript = collections.namedtuple("OpenTypeScript", "tag name deprecated")
OpenTypeSystem = collections.namedtuple("OpenTypeSystem", "tag codes name deprecated")


def opentype():
    """Returns the script tags and the language systems, in registry order."""
    scripts, systems = [], []
    with open(OPENTYPE, encoding="utf-8") as f:
        for line in f:
            if line.startswith("#") or not line.strip():
                continue
            fields = line.rstrip("\n").split("\t")
            deprecated = fields[-1] == "deprecated"
            if fields[0] == "script":
                scripts.append(OpenTypeScript(fields[1], fields[2], deprecated))
            else:
                systems.append(OpenTypeSystem(fields[1], [c for c in fields[2].split(",") if c], fields[3], deprecated))
    return scripts, systems


def pad(tag):
    return tag.ljust(4)


def tag_value(tag):
    return int.from_bytes(pad(tag).encode("ascii"), "big")


# ----------------------------------------------------------------------------------------------------------------------
# Data that the engine derives from the sources

def name_keys(name):
    """Returns the ways in which a name may be written: the name itself, each parenthesized alternative, and the name
    without them, each reduced to lowercase letters."""
    names = [name] + re.findall(r"\(([^)]*)\)", name) + [re.sub(r"\s*\([^)]*\)", "", name)]
    return {re.sub(r"[^a-z]", "", n.lower()) for n in names}


def script_opentype_tags(reg, scripts):
    """Returns the OpenType tags of each ISO 15924 script, newest first and deprecated last. A tag belongs to a script
    if the tag is the script's code in lowercase, or if the tag's name, without a version suffix ("v.2"), names the
    script as the registry does. A tag whose name lists several scripts ("Hiragana and Katakana") belongs to each of
    them. A tag for no ISO 15924 script (default, math and music) is the shaper's own, and is returned as unmatched."""
    by_name = {}
    for code in reg.scripts:
        for name in reg.script_name[code]:
            for key in name_keys(name):
                by_name.setdefault(key, code)
    out = collections.defaultdict(list)
    unmatched = []
    for script in scripts:
        base = re.sub(r"\s*v\.\d+$", "", script.name)
        codes = set()
        if script.tag.title() in reg.script_index:
            codes.add(script.tag.title())
        for part in [base] + re.split(r",\s*|\s+and\s+", base):
            code = next((by_name[k] for k in sorted(name_keys(part)) if k in by_name), None)
            if code is not None:
                codes.add(code)
                if part == base:
                    break
        if not codes:
            unmatched.append(script.tag)
        for code in codes:
            if all(s.tag != script.tag for s in out[code]):
                out[code].append(script)
    return {code: [s.tag for s in sorted(tags, key=lambda s: (s.deprecated, not s.tag[-1].isdigit()))]
            for code, tags in out.items()}, unmatched


# The language systems that the registry names for a script or a region of their language, keyed by tag, with the
# script and region that the system's name qualifies it by. Such a system applies only to a locale with that script
# and, if it has one, that region. The qualifier of a system that the registry names with an ISO 15924 code
# ("equivalent to ISO 15924 'Syre'") is read from its name instead.
QUALIFIED = {
    "ZHS": ("Hans", None),      # Chinese, Simplified
    "ZHT": ("Hant", None),      # Chinese, Traditional
    "ZHH": ("Hant", "HK"),      # Chinese, Traditional, Hong Kong SAR
    "ZHTM": ("Hant", "MO"),     # Chinese, Traditional, Macao SAR
    "ZHP": ("Bopo", None),      # Chinese, Phonetic: written in Bopomofo
    "KGE": ("Geok", None),      # Khutsuri Georgian
}

# Systems for a variant, which the engine does not distinguish, so they are left out.
VARIANT_SYSTEMS = {"PGR"}       # Polytonic Greek


def qualifier(system):
    m = re.search(r"ISO 15924 '([A-Z][a-z]{3})'", system.name)
    return (m.group(1), None) if m else QUALIFIED.get(system.tag, (None, None))


def language_systems(reg, part1, systems, used):
    """Returns the OpenType language systems of each canonical language, as (tag, script, region), most specific
    first. The systems that list the language come first, ordered with the system that lists the fewest languages
    first and deprecated tags last. Then come the systems that list its macrolanguage, as a fallback. For a
    macrolanguage that no system lists, the systems are those of its member languages, the most used first. `used`
    counts the people using each language."""
    listed = collections.defaultdict(list)
    for system in systems:
        if system.tag in VARIANT_SYSTEMS:
            continue
        for code in system.codes:
            language = reg.language(part1.get(code, code))
            if language is not None and system not in listed[language]:
                listed[language].append(system)
    def ordered(language):
        return [s for s in sorted(listed.get(language, []), key=lambda s: (s.deprecated, len(s.codes)))]
    members = collections.defaultdict(list)
    for language, macro in reg.macrolanguage.items():
        members[macro].append(language)
    out = {}
    for language in reg.languages:
        chosen = ordered(language)
        if language in reg.macrolanguage:
            chosen += [s for s in ordered(reg.macrolanguage[language]) if s not in chosen]
        if not chosen and language in members:
            for member in sorted(members[language], key=lambda m: -used.get(m, 0)):
                chosen += [s for s in ordered(member) if s not in chosen]
        if chosen:
            out[language] = [(s.tag,) + qualifier(s) for s in chosen]
    return out


def cldr_language_aliases(reg):
    """Returns CLDR's aliases of language subtags, each mapped to the locale that replaces it (UTS #35's canonical
    form), in addition to the IANA registry's preferred values. For example, `cmn` maps to `zh` and `sh` to
    `sr-Latn`."""
    with open(os.path.join(CLDR, "common", "supplemental", "supplementalMetadata.xml"), encoding="utf-8") as f:
        text = re.sub(r"<!--.*?-->", "", f.read(), flags=re.S)
    out = {}
    for alias, replacement in re.findall(r'<languageAlias type="([a-z]{2,3})" replacement="([^" ]+)"', text):
        language, script, region = cldr_locale(replacement)
        language = reg.language(language)
        if language is not None and alias != language:
            out[alias] = (language, script, region)
    return out


class Likely:
    """CLDR's likely subtags, and UTS #35's "Add Likely Subtags" applied to them, as lib/locale resolves a locale."""

    def __init__(self, reg):
        self.table = {}
        for source, target in likely_subtags():
            self.table[source] = target

    def maximize(self, language, script, region):
        for key in ((language, script, region), (language, None, region), (language, script, None),
                    (language, None, None)) + ((("und", script, None),) if script is not None else ()):
            match = self.table.get(key)
            if match is not None:
                return (language if language != "und" else match[0], script or match[1], region or match[2])
        return (language, script, region)


def select_systems(systems, script, region):
    """Returns the systems of a language that apply to a locale of `script` and `region`, in the order in which
    lib/locale chooses them: those qualified by both, then those qualified by the script alone, then those not
    qualified."""
    both = [t for t, s, r in systems if s is not None and r is not None and s == script and r == region]
    alone = [t for t, s, r in systems if s is not None and r is None and s == script]
    plain = [t for t, s, r in systems if s is None]
    return both + alone + plain


def harfbuzz_languages():
    """Returns HarfBuzz's OpenType languages for each BCP 47 language, in HarfBuzz's order, for --check."""
    with open(HARFBUZZ_TAGS, encoding="utf-8") as f:
        text = f.read()
    out = collections.defaultdict(list)
    for name in ("ot_languages2", "ot_languages3"):
        start = text.index("static const LangTag %s[]" % name)
        body = text[start:text.index("};", start)]
        for language, tag in re.findall(r"\{HB_TAG\(([^)]*)\),\s*HB_TAG\(([^)]*)\)\}", body):
            subtag = "".join(c.strip("'") for c in language.split(",")).strip()
            system = "".join(c.strip("'") for c in tag.split(",")).rstrip()
            out[subtag].append(system)
    return out


def check(reg, systems_by_language, aliases):
    hb = harfbuzz_languages()
    likely = Likely(reg)
    same = differ = fallback = 0
    for subtag in sorted(set(hb) | set(systems_by_language)):
        language, script, region = aliases.get(subtag, (reg.language(subtag), None, None))
        if language is None:
            continue
        _, script, region = likely.maximize(language, script, region)
        ours = select_systems(systems_by_language.get(language, []), script, region)
        theirs = hb.get(subtag, [])
        # If HarfBuzz's table has no row for a three-letter language, HarfBuzz derives its tag from its code.
        if not theirs and len(language) == 3:
            theirs = [language.upper()]
        if ours == theirs:
            same += 1
            continue
        if ours[:1] == theirs[:1]:
            fallback += 1
            continue
        differ += 1
        print("  %-4s %s-%s ours %-30s harfbuzz %s" % (subtag, script, region, ",".join(ours), ",".join(theirs)))
    print("%d languages agree with HarfBuzz, %d more in their first system and not their fallbacks, and %d differ "
          "in their first" % (same, fallback, differ))


# ----------------------------------------------------------------------------------------------------------------------
# The data that lib/locale reads

LANGUAGE_BITS, REGION_BITS, SCRIPT_BITS = 14, 9, 8


class Model:
    """The engine's languages, scripts and regions as values, and the data that the engine derives for each from the
    sources."""

    def __init__(self):
        self.reg = registry()
        self.part1 = iso_639_3()
        self.used = collections.Counter()
        for tag, people in populations().items():
            self.used[tag.split("_")[0]] += people
        ot_scripts, ot_systems = opentype()
        self.script_tags, self.unmatched_scripts = script_opentype_tags(self.reg, ot_scripts)
        self.systems = language_systems(self.reg, self.part1, ot_systems, self.used)
        self.aliases = cldr_language_aliases(self.reg)
        assert len(self.reg.languages) < (1 << LANGUAGE_BITS) and len(self.reg.regions) < (1 << REGION_BITS)
        assert len(self.reg.scripts) <= (1 << SCRIPT_BITS)
        self.likely = self.likely_table()

    def locale(self, language, script, region):
        """Returns a locale's packed value, with an absent script as Zzzz (0) and an absent region as None (0)."""
        value = self.reg.language_index[language] if language else 0
        value |= (self.reg.region_index[region] if region else 0) << LANGUAGE_BITS
        value |= (self.reg.script_index[script] if script else 0) << (LANGUAGE_BITS + REGION_BITS)
        return value

    def canonical(self, language, script, region):
        """Returns a CLDR locale in the registry's terms, or None if the engine does not name one of its subtags."""
        language = self.reg.language(language)
        if language is None or (script is not None and script not in self.reg.script_index):
            return None
        if region is not None:
            region = self.reg.region(region)
            if region is None:
                return None
        return language, script, region

    def likely_table(self):
        """Returns CLDR's likely subtags in the registry's terms, mapping each locale to its maximized form. If CLDR has
        no row for a language, the language's Suppress-Script is used."""
        out = {}
        for source, target in likely_subtags():
            source, target = self.canonical(*source), self.canonical(*target)
            if source is not None and target is not None:
                out.setdefault(source, target)
        for language, script in self.reg.suppress_script.items():
            language = self.reg.language(language)
            if language is not None and script in self.reg.script_index:
                out.setdefault((language, None, None), (language, script, None))
        return out

    def maximize(self, language, script, region):
        for key in ((language, script, region), (language, None, region), (language, script, None),
                    (language, None, None)) + ((("und", script, None),) if script is not None else ()):
            match = self.likely.get(key)
            if match is not None:
                return (language if language != "und" else match[0], script or match[1], region or match[2])
        return (language, script, region)

    def common_languages(self):
        """Returns the languages that pr/locale_common.h names: those that CLDR counts at least COMMON_POPULATION
        people as using, and the most used language of each script that UAX #31 recommends. A script that is written
        together with others counts as their combination."""
        used = collections.Counter()
        for tag, people in self.used.items():
            language = self.reg.language(tag)
            if language is not None:
                used[language] += people
        common = {language for language, people in used.items() if people >= COMMON_POPULATION}
        for script in recommended_scripts():
            scripts = COMBINED.get(script, (script,))
            speakers = [(people, language) for language, people in used.items()
                        if self.maximize(language, None, None)[1] in scripts]
            if speakers:
                common.add(max(speakers)[1])
        return sorted(common)

    def quotes(self):
        """Returns the quotation marks of CLDR's root, and the marks of each locale whose marks differ from those of
        its fallback, by maximized locale. Marks are (open, close, inner open, inner close). A locale falls back from
        language, script and region to language and script, then to the root."""
        own = delimiters()
        parents = parent_locales()
        fields = ("quotationStart", "quotationEnd", "alternateQuotationStart", "alternateQuotationEnd")

        def parent(name):
            if name in parents:
                return parents[name]
            return name.rsplit("_", 1)[0] if "_" in name else "root"

        def resolved(name):
            marks = []
            for field in fields:
                at = name
                while field not in own.get(at, {}) and at != "root":
                    at = parent(at)
                marks.append(own["root"][field])
                if field in own.get(at, {}):
                    marks[-1] = own[at][field]
            return tuple(marks)

        root = resolved("root")
        entries = {}
        explicit = {}
        names = [n[:-4] for n in os.listdir(MAIN) if n.endswith(".xml") and n not in ("root.xml",)]
        for name in names:
            locale = self.canonical(*cldr_locale(name))
            if locale is None:
                continue
            key = self.maximize(*locale)
            given = sum(part is not None for part in locale)
            if key not in entries or given > explicit[key]:
                entries[key], explicit[key] = resolved(name), given
            # The marks of a language and script come from the language's own locale if the script is the language's
            # likely script, and otherwise from the locale that names the script.
            pair = (key[0], key[1], None)
            if locale[2] is None and (pair not in entries or given > explicit[pair]):
                entries[pair], explicit[pair] = resolved(name), given
        kept = {}
        for key, marks in entries.items():
            fallback = entries.get((key[0], key[1], None), root) if key[2] is not None else root
            if marks != fallback:
                kept[key] = marks
        return root, kept


# ----------------------------------------------------------------------------------------------------------------------
# Emitting

LOCALE = os.path.join(ROOT, "lib", "locale")
SOURCES = ["the IANA Language Subtag Registry and ISO 639-3 as CLDR keeps them", "CLDR",
           "UAX #31's Table 5", "tools/gen/language/opentype.txt"]


def generated():
    return gen_common.banner(__file__, *SOURCES)


def ident(text):
    return text[:1].upper() + text[1:]


def common_scripts(m):
    """Returns the scripts that pr/locale_common.h names: those that UAX #31 recommends, the combinations that they
    are written in, and ISO 15924's special codes (Common, Inherited, mathematical notation, symbols and emoji, and
    unwritten text), except Zzzz, which pr/locale.h names."""
    combined = {c for codes in COMBINED.values() for c in codes} | {"Hrkt"}
    special = SPECIAL_SCRIPTS - {"Zzzz"}
    return [c for c in m.reg.scripts if c in set(recommended_scripts()) | combined | special]


def public_header(m):
    return [generated(), """
// Languages, scripts and regions, as BCP 47 language tags name them, and the locales that tags resolve to. Each is a value of a closed set: a language or region the IANA Language
// Subtag Registry lists, or an ISO 15924 script, but none of their private-use ranges. This header declares their
// types and functions: pr/locale_common.h names every region and the most used languages and scripts, and
// pr/locale_full.h names the rest. Every function is thread-safe.

#pragma once

#include "pr/base.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A script, by its ISO 15924 code. Zzzz, the code for an unknown script, is zero, and stands for no script in a
// locale.
enum { PrScript_Zzzz };
typedef uint8_t PrScript;

// A language, by its subtag in the IANA Language Subtag Registry: ISO 639-1 where it has a code, else ISO 639-2 or
// 639-3. A deprecated subtag is the one the registry replaces it with. None is zero.
enum { PrLanguage_None };
typedef uint16_t PrLanguage;

// A region, by its subtag in the registry: ISO 3166-1 alpha-2, or UN M.49 numeric. None is zero.
enum { PrRegion_None };
typedef uint16_t PrRegion;

// A locale: a language, a script and a region, packed. Zero is no locale.
typedef uint32_t PrLocale;

// The bits of a locale each part takes, from the lowest: the language, the region, then the script.
enum {
    kPrLocaleLanguageBits = %d,
    kPrLocaleRegionBits = %d,
    kPrLocaleScriptBits = %d,
};

// The longest tag prLocaleTag writes: a language of three letters, a script and a region of three digits.
enum { kPrLocaleMaxTag = 12 };

// The most OpenType tags a script has (prScriptOpenTypeTags).
enum { kPrScriptMaxOpenTypeTags = 2 };

// Returns the locale of `language`, `script` and `region`.
PR_CONSTEXPR PrLocale prLocaleMake(PrLanguage language, PrScript script, PrRegion region) {
    return PR_VALUE_CAST(PrLocale, language) | (PR_VALUE_CAST(PrLocale, region) << kPrLocaleLanguageBits)
           | (PR_VALUE_CAST(PrLocale, script) << (kPrLocaleLanguageBits + kPrLocaleRegionBits));
}

// Returns a part of a locale.
PR_CONSTEXPR PrLanguage prLocaleLanguage(PrLocale locale) {
    return PR_VALUE_CAST(PrLanguage, locale & ((1u << kPrLocaleLanguageBits) - 1));
}
PR_CONSTEXPR PrRegion prLocaleRegion(PrLocale locale) {
    return PR_VALUE_CAST(PrRegion, (locale >> kPrLocaleLanguageBits) & ((1u << kPrLocaleRegionBits) - 1));
}
PR_CONSTEXPR PrScript prLocaleScript(PrLocale locale) {
    return PR_VALUE_CAST(PrScript, locale >> (kPrLocaleLanguageBits + kPrLocaleRegionBits));
}

// Returns the language, script or region a subtag names, matched ASCII case-insensitively, a deprecated subtag as the
// registry replaces it. Returns None, Zzzz or None for any other text.
PrLanguage prLanguageFromSubtag(PrStringView subtag);
PrScript prScriptFromCode(PrStringView code);
PrRegion prRegionFromSubtag(PrStringView subtag);

// Returns the subtag of a language, script or region as BCP 47 writes it: lowercase, titlecase and uppercase. Returns
// the empty view for None.
PrStringView prLanguageSubtag(PrLanguage language);
PrStringView prScriptCode(PrScript script);
PrStringView prRegionSubtag(PrRegion region);

// Returns the locale a BCP 47 language tag resolves to (RFC 5646): matched ASCII case-insensitively, canonical by the
// registry's preferred values and CLDR's aliases, and with its likely script and region added (UTS #35, "Add Likely
// Subtags"). The first subtag that is ill-formed or that the registry does not list ends the tag, as RFC 4647's lookup
// ends it, so `en-EN` resolves to `en`. Variants, extensions and private use are dropped, and so is a private-use
// script or region. Returns zero for a tag whose language does not resolve.
PrLocale prLocaleFromTag(PrStringView tag);

// Writes the tag of a locale, such as `zh-Hant-TW`, to `out` and returns its length: its language, then its script
// where it has one, then its region where it has one. Writes nothing for zero.
uint32_t prLocaleTag(PrLocale locale, char out[PR_NONNULL kPrLocaleMaxTag]);

// Writes the OpenType script tags of a script to `out`, the newest first, and returns their count: none for a script
// that the OpenType registry gives no tag.
uint32_t prScriptOpenTypeTags(PrScript script, uint32_t out[PR_NONNULL kPrScriptMaxOpenTypeTags]);

// Writes up to `capacity` of the OpenType language system tags for a locale's text to `out`, the most specific first,
// and returns their total: a system for its script and region before one for its script, then one for its language,
// then its macrolanguage's.
uint32_t prLocaleOpenTypeLanguages(PrLocale locale, uint32_t* PR_NULLABLE out, uint32_t capacity);

// Returns a locale's quotation mark, as CLDR gives it: the outer pair at depth 0 and the inner pair deeper, the
// opening mark if `open` is set. A locale CLDR has no marks for takes CLDR's root marks.
PrStringView prLocaleQuote(PrLocale locale, uint32_t depth, bool open);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
""" % (LANGUAGE_BITS, REGION_BITS, SCRIPT_BITS)]


def names_header(m, which):
    if which == "common":
        title = "every region, and the most used languages and scripts"
        languages = m.common_languages()
        regions = m.reg.regions
        scripts = common_scripts(m)
        note = ("the languages CLDR counts at least %d million people using, the most used language of each script "
                "that UAX #31 recommends for modern use, those scripts and the combinations they are written in, "
                "and ISO 15924's special codes" % (COMMON_POPULATION // 1000000))
        includes = ['#include "pr/locale.h"\n']
    else:
        title = "the languages and scripts locale_common.h does not name"
        common = set(m.common_languages())
        languages = [l for l in m.reg.languages if l not in common]
        regions = []
        scripts = [c for c in m.reg.scripts if c not in set(common_scripts(m)) and c != "Zzzz"]
        note = ("each other language subtag the IANA Language Subtag Registry lists, and each other ISO 15924 script, "
                "but their private-use ranges")
        includes = ['#include "pr/locale.h"\n', '#include "pr/locale_common.h"\n']
    out = [generated(),
           "\n// The names of %s: %s. C's enums are unscoped, so the names of one set are split across headers without "
           "loss.\n\n#pragma once\n\n" % (title, note)] + includes
    out.append("\nPR_ASSUME_NONNULL_BEGIN\n")
    out.append("\n// The languages, by their subtags.\nenum {\n")
    out += gen_common.enum_members([("PrLanguage_%s = %d" % (ident(l), m.reg.language_index[l]),
                                     m.reg.description[l]) for l in languages])
    out.append("};\n")
    out.append("\n// The scripts, by their ISO 15924 codes.\nenum {\n")
    out += gen_common.enum_members([("PrScript_%s = %d" % (c, m.reg.script_index[c]), m.reg.script_name[c][0])
                                    for c in scripts])
    out.append("};\n")
    if regions:
        out.append("\n// The regions, by their subtags.\nenum {\n")
        out += gen_common.enum_members([("PrRegion_%s = %d" % (r, m.reg.region_index[r]), "") for r in regions])
        out.append("};\n")
    out.append("\nPR_ASSUME_NONNULL_END\n")
    return out


DATA_HEADER = """
// The tables lib/locale/src/locale.cpp resolves language tags with, and the lookups over them.

#pragma once

#include "pr/locale.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The number of scripts: every ISO 15924 code but the private-use ones.
constexpr uint32_t kScriptCount = %d;

// Returns the language a subtag names, matched ASCII case-insensitively, a deprecated subtag as the registry replaces
// it, or None for any other text.
PrLanguage languageFromSubtag(PrStringView subtag);

// Returns true if `subtag` is an extlang, and writes the language of its prefix to `*prefix`. An extlang is also a
// language subtag, which replaces the pair (RFC 5646 section 4.5).
bool languageFromExtlang(PrStringView subtag, PrLanguage* prefix);

// Returns true if `code` is a script the engine names, and writes it to `*script`.
bool scriptFromCode(PrStringView code, PrScript* script);

// Returns the region a subtag names, a deprecated subtag as the registry replaces it, or None for any other text.
PrRegion regionFromSubtag(PrStringView subtag);

// Returns true if the registry lists `tag` whole, as a grandfathered or redundant tag that another replaces, and writes
// that tag to `*replacement`: the empty view where no tag replaces it.
bool localeWholeTagReplacement(PrStringView tag, PrStringView* replacement);

// Returns the locale CLDR's aliases replace a language with (UTS #35), or zero where they do not.
PrLocale languageAlias(PrLanguage language);

// Returns the maximized locale that the likely subtags give `key`, or zero where they give it none.
PrLocale localeLikelySubtags(PrLocale key);

// Returns the subtag of a language, script or region as BCP 47 writes it.
PrStringView languageSubtag(PrLanguage language);
PrStringView scriptCode(PrScript script);
PrStringView regionSubtag(PrRegion region);

// Each script's OpenType tags, the newest first, and zero past the last.
extern const uint32_t kScriptOpenType[kScriptCount][kPrScriptMaxOpenTypeTags];

// Writes the range of a language's OpenType language systems in kSystemTag and kSystemQualifier to `*first` and
// `*end`, which is empty for a language with none.
void languageSystems(PrLanguage language, uint32_t* first, uint32_t* end);

// Each OpenType language system's tag, and the script and region it is for, as a locale with no language: its script
// Zzzz where it is for any script, and its region None where it is for any region.
extern const uint32_t kSystemTag[];
extern const PrLocale kSystemQualifier[];

// Returns the quotation marks of a locale's entry, or null where it has none: the outer opening and closing marks,
// then the inner. A locale without an entry takes its fallback's marks.
const PrStringView* PR_NULLABLE localeQuoteMarks(PrLocale key);

// Returns CLDR's root quotation marks, which a locale whose fallbacks have none takes.
const PrStringView* localeRootQuoteMarks();

} // namespace pr_impl

PR_ASSUME_NONNULL_END
"""


def sorted_table(name, pairs, value_type, comment):
    pairs = sorted(pairs)
    return ["\n// %s\n" % comment,
            gen_common.table("constexpr uint32_t %sKey[]" % name, ["%du" % k for k, _ in pairs], 8),
            gen_common.table("constexpr %s %sValue[]" % (value_type, name), [str(v) for _, v in pairs], 8)]


def search(name, key, absent):
    """Returns the lines of a binary search of a sorted table's keys for `key`. The lines return the key's value, or
    `absent` if the key is not found."""
    return ("    uint32_t at = lowerBound(Span(%(n)sKey), %(k)s);\n"
            "    return at < countOf(%(n)sKey) && %(n)sKey[at] == %(k)s ? %(n)sValue[at] : %(a)s;\n"
            % {"n": name, "k": key, "a": absent})


def data_source(m):
    from tools.gen.name_table import Blob, NameTable
    reg = m.reg
    out = [generated(), '\n#include "locale/src/locale_data.h"\n\n#include "pr/count_of.h"\n'
           '#include "pr/name_table.h"\n#include "pr/search.h"\n\nnamespace pr_impl {\nnamespace {\n']

    # Each table's names are the names of its values, in order, starting from None, followed by the aliases.
    aliases = sorted(reg.language_alias)
    languages = NameTable("kLanguageName", [""] + reg.languages + aliases, lower=True)
    values = ([0] + [reg.language_index[l] for l in reg.languages]
              + [reg.language_index.get(reg.language(a), 0) for a in aliases])
    out += [languages.tables(), gen_common.table("constexpr uint16_t kLanguageValue[]", values, 16)]

    extlangs = sorted(reg.extlang)
    extlang = NameTable("kExtlangName", [""] + extlangs, lower=True)
    out += [extlang.tables(), gen_common.table(
        "constexpr uint16_t kExtlangPrefix[]",
        [0] + [reg.language_index.get(reg.language(reg.extlang[e][0]), 0) for e in extlangs], 16)]

    scripts = NameTable("kScriptName", [c.lower() for c in reg.scripts], lower=True)
    out += [scripts.tables(), Blob("kScriptCode", reg.scripts).tables()]

    region_aliases = sorted(reg.region_alias)
    regions = NameTable("kRegionName", [""] + [r.lower() for r in reg.regions + region_aliases], lower=True)
    out += [regions.tables(), Blob("kRegionCode", [""] + reg.regions).tables(),
            gen_common.table("constexpr uint16_t kRegionValue[]",
                             [0] + [reg.region_index[r] for r in reg.regions]
                             + [reg.region_index.get(reg.region(a), 0) for a in region_aliases], 16)]

    whole = sorted(reg.whole)
    wholes = NameTable("kWholeTagName", [""] + whole, lower=True)
    out += [wholes.tables(), Blob("kWholeTagReplacement", [""] + [reg.whole[t] or "" for t in whole]).tables()]

    out += sorted_table("kLanguageAlias",
                        [(reg.language_index[reg.language(a)], m.locale(*t)) for a, t in m.aliases.items()
                         if reg.language(a) is not None and reg.language(a) != t[0]],
                        "uint32_t", "CLDR's language aliases, by the language they replace.")
    out += sorted_table("kLikely", [(m.locale(*k), m.locale(*v)) for k, v in m.likely.items()], "uint32_t",
                        "The likely subtags, by the locale they maximize.")

    tags, qualifiers, starts = [], [], []
    for language in reg.languages:
        if language in m.systems:
            starts.append((reg.language_index[language], len(tags)))
            for tag, script, region in m.systems[language]:
                tags.append("0x%08xu" % tag_value(tag))
                qualifiers.append("%du" % m.locale(None, script, region))
    out += ["\n// The languages with OpenType language systems, and where the systems of each start.\n",
            gen_common.table("constexpr uint32_t kSystemsKey[]", ["%du" % k for k, _ in starts], 12),
            gen_common.table("constexpr uint16_t kSystemsAt[]", [a for _, a in starts] + [len(tags)], 12)]

    root, quotes = m.quotes()
    marks = sorted({mark for value in [root] + list(quotes.values()) for mark in value})
    sets = [root] + sorted(set(quotes.values()) - {root})
    out += [Blob("kQuoteMark", marks).tables(),
            "\n// The sets of quotation marks, the root's first, each by its marks in kQuoteMark.\n",
            gen_common.table("constexpr uint8_t kQuoteSet[][4]",
                             ["{%s}" % ", ".join(str(marks.index(x)) for x in q) for q in sets], 6)]
    out += sorted_table("kQuote", [(m.locale(*k), sets.index(v)) for k, v in quotes.items()], "uint8_t",
                        "The locales whose quotation marks differ from those of their fallback, and their sets.")
    out.append("\n" + "// The views of each set's marks, made on first use, which is thread-safe.\nconst PrStringView* quoteSets() {\n    static const PrStringView* sets = [] {\n        static PrStringView views[countOf(kQuoteSet)][4];\n        for (uint32_t set = 0; set < countOf(kQuoteSet); ++set) {\n            for (uint32_t mark = 0; mark < 4; ++mark)\n                views[set][mark] = nameAt(kQuoteMarkBlob, kQuoteMarkAt, kQuoteSet[set][mark]);\n        }\n        return &views[0][0];\n    }();\n    return sets;\n}\n")
    out.append("\n} // namespace\n")

    open_type = []
    for code in reg.scripts:
        t = m.script_tags.get(code, [])[:2]
        open_type.append("{%s}" % ", ".join(["0x%08xu" % tag_value(x) for x in t] + ["0"] * (2 - len(t))))
    out += [gen_common.table("extern const uint32_t kScriptOpenType[kScriptCount][kPrScriptMaxOpenTypeTags]",
                             open_type, 4),
            gen_common.table("extern const uint32_t kSystemTag[]", tags, 6),
            gen_common.table("extern const PrLocale kSystemQualifier[]", qualifiers, 8)]

    out.append("\nPrLanguage languageFromSubtag(PrStringView subtag) {\n" + languages.lookup("subtag", "index")
               + "    return %s ? PrLanguage(kLanguageValue[index]) : PrLanguage_None;\n}\n"
               % languages.confirm("subtag", languages.name("index")))
    out.append("\nbool languageFromExtlang(PrStringView subtag, PrLanguage* prefix) {\n" + extlang.lookup("subtag", "index")
               + "    if (index == 0 || !%s)\n        return false;\n    *prefix = kExtlangPrefix[index];\n"
               "    return true;\n}\n" % extlang.confirm("subtag", extlang.name("index")))
    out.append("\nbool scriptFromCode(PrStringView code, PrScript* script) {\n" + scripts.lookup("code", "index")
               + "    if (!%s)\n        return false;\n    *script = PrScript(index);\n    return true;\n}\n"
               % scripts.confirm("code", scripts.name("index")))
    out.append("\nPrRegion regionFromSubtag(PrStringView subtag) {\n" + regions.lookup("subtag", "index")
               + "    return %s ? PrRegion(kRegionValue[index]) : PrRegion_None;\n}\n"
               % regions.confirm("subtag", regions.name("index")))
    out.append("\nbool localeWholeTagReplacement(PrStringView tag, PrStringView* replacement) {\n"
               + wholes.lookup("tag", "index")
               + "    if (index == 0 || !%s)\n        return false;\n"
               "    *replacement = nameAt(kWholeTagReplacementBlob, kWholeTagReplacementAt, index);\n"
               "    return true;\n}\n" % wholes.confirm("tag", wholes.name("index")))
    out.append("\nPrLocale languageAlias(PrLanguage language) {\n"
               + search("kLanguageAlias", "uint32_t(language)", "0u") + "}\n")
    out.append("\nPrLocale localeLikelySubtags(PrLocale key) {\n" + search("kLikely", "key", "0u") + "}\n")
    out.append("\nPrStringView languageSubtag(PrLanguage language) {\n    return %s;\n}\n"
               % languages.name("language"))
    out.append("\nPrStringView scriptCode(PrScript script) {\n"
               "    return nameAt(kScriptCodeBlob, kScriptCodeAt, script);\n}\n")
    out.append("\nPrStringView regionSubtag(PrRegion region) {\n"
               "    return nameAt(kRegionCodeBlob, kRegionCodeAt, region);\n}\n")
    out.append("""
void languageSystems(PrLanguage language, uint32_t* first, uint32_t* end) {
    uint32_t at = lowerBound(Span(kSystemsKey), uint32_t(language));
    bool found = at < countOf(kSystemsKey) && kSystemsKey[at] == language;
    *first = found ? kSystemsAt[at] : 0;
    *end = found ? kSystemsAt[at + 1] : 0;
}

const PrStringView* PR_NULLABLE localeQuoteMarks(PrLocale key) {
    uint32_t at = lowerBound(Span(kQuoteKey), key);
    if (at >= countOf(kQuoteKey) || kQuoteKey[at] != key)
        return nullptr;
    return quoteSets() + 4 * kQuoteValue[at];
}

const PrStringView* localeRootQuoteMarks() {
    return quoteSets();
}
""")
    out.append("\n} // namespace pr_impl\n")
    return out


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--refresh", action="store_true", help="read the OpenType registries again")
    parser.add_argument("--check", action="store_true", help="compare the OpenType languages with HarfBuzz's")
    args = parser.parse_args()
    if args.refresh:
        refresh_opentype()
    m = Model()
    print("registry %s: %d languages, %d scripts, %d regions; %d common languages; %d scripts with OpenType tags, "
          "%d tags for no script %s" % (m.reg.date, len(m.reg.languages), len(m.reg.scripts), len(m.reg.regions),
                                        len(m.common_languages()), len(m.script_tags), len(m.unmatched_scripts),
                                        m.unmatched_scripts))
    if args.check:
        check(m.reg, m.systems, m.aliases)
        return
    include = os.path.join(LOCALE, "include", "pr")
    gen_common.write(os.path.join(include, "locale.h"), public_header(m))
    gen_common.write(os.path.join(include, "locale_common.h"), names_header(m, "common"))
    gen_common.write(os.path.join(include, "locale_full.h"), names_header(m, "full"))
    gen_common.write(os.path.join(LOCALE, "src", "locale_data.h"),
                     [generated(), DATA_HEADER.replace("%d", str(len(m.reg.scripts)), 1)])
    gen_common.write(os.path.join(LOCALE, "src", "locale_data.cpp"), data_source(m))


if __name__ == "__main__":
    main()
