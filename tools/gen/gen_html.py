#!/usr/bin/env python3
"""Code generator for the tables that html/#parsing reads.

Reads the WHATWG entities.json and the W3C webref "ed" (editor's drafts) definition extracts, and emits:

    html_entity.cpp         every named character reference, and the lookup that pr/html_entity.h declares
    html_adjust.h/.cpp    the foreign attribute names that html/#parsing-main-inforeign respells

Usage: python3 -m tools.gen.gen_html [--entities FILE] [--webref DIR] [--out DIR] [--entity-out DIR]
"""

import argparse
import json
import os
import sys

from tools.gen import gen_common
from tools.gen import name_table
from tools.gen.gen_common import namespaced, table, write


# html/#parsing-main-inforeign "adjust SVG attributes": the SVG attributes whose names are not all lowercase, keyed by
# the name that the tokenizer produced. Parsing follows the parsing table. webref is read only to check the spellings
# that it carries.
ADJUST_SVG = {
    "attributename": "attributeName",
    "attributetype": "attributeType",
    "basefrequency": "baseFrequency",
    "baseprofile": "baseProfile",
    "calcmode": "calcMode",
    "clippathunits": "clipPathUnits",
    "diffuseconstant": "diffuseConstant",
    "edgemode": "edgeMode",
    "filterunits": "filterUnits",
    "glyphref": "glyphRef",
    "gradienttransform": "gradientTransform",
    "gradientunits": "gradientUnits",
    "kernelmatrix": "kernelMatrix",
    "kernelunitlength": "kernelUnitLength",
    "keypoints": "keyPoints",
    "keysplines": "keySplines",
    "keytimes": "keyTimes",
    "lengthadjust": "lengthAdjust",
    "limitingconeangle": "limitingConeAngle",
    "markerheight": "markerHeight",
    "markerunits": "markerUnits",
    "markerwidth": "markerWidth",
    "maskcontentunits": "maskContentUnits",
    "maskunits": "maskUnits",
    "numoctaves": "numOctaves",
    "pathlength": "pathLength",
    "patterncontentunits": "patternContentUnits",
    "patterntransform": "patternTransform",
    "patternunits": "patternUnits",
    "pointsatx": "pointsAtX",
    "pointsaty": "pointsAtY",
    "pointsatz": "pointsAtZ",
    "preservealpha": "preserveAlpha",
    "preserveaspectratio": "preserveAspectRatio",
    "primitiveunits": "primitiveUnits",
    "refx": "refX",
    "refy": "refY",
    "repeatcount": "repeatCount",
    "repeatdur": "repeatDur",
    "requiredextensions": "requiredExtensions",
    "requiredfeatures": "requiredFeatures",
    "specularconstant": "specularConstant",
    "specularexponent": "specularExponent",
    "spreadmethod": "spreadMethod",
    "startoffset": "startOffset",
    "stddeviation": "stdDeviation",
    "stitchtiles": "stitchTiles",
    "surfacescale": "surfaceScale",
    "systemlanguage": "systemLanguage",
    "tablevalues": "tableValues",
    "targetx": "targetX",
    "targety": "targetY",
    "textlength": "textLength",
    "viewbox": "viewBox",
    "viewtarget": "viewTarget",
    "xchannelselector": "xChannelSelector",
    "ychannelselector": "yChannelSelector",
    "zoomandpan": "zoomAndPan",
}

# "adjust MathML attributes", which covers one attribute. MathML element names are all lowercase, so there is no tag
# table for them.
ADJUST_MATHML = {
    "definitionurl": "definitionURL",
}

# "adjust foreign attributes": a prefixed name becomes a namespace and a local name.
FOREIGN = {
    "xlink:actuate": ("XLink", "actuate"),
    "xlink:arcrole": ("XLink", "arcrole"),
    "xlink:href": ("XLink", "href"),
    "xlink:role": ("XLink", "role"),
    "xlink:show": ("XLink", "show"),
    "xlink:title": ("XLink", "title"),
    "xlink:type": ("XLink", "type"),
    "xml:lang": ("Xml", "lang"),
    "xml:space": ("Xml", "space"),
    "xmlns": ("XmlNs", "xmlns"),
    "xmlns:xlink": ("XmlNs", "xlink"),
}

def adjusted_tags(webref):
    """Returns the table for html/#parsing-main-inforeign "adjust SVG tag name": the SVG elements whose names are not
    all lowercase, keyed by the name that the tokenizer produced. webref spells an element as its spec does, so the
    table comes from those spellings."""
    spelled = {}
    for spec in SVG_SPECS:
        path = os.path.join(webref, "ed", "elements", spec + ".json")
        if not os.path.exists(path):
            continue
        with open(path, encoding="utf-8") as f:
            for element in json.load(f)["elements"]:
                if element["name"].lower() != element["name"]:
                    spelled[element["name"].lower()] = element["name"]
    if not spelled:
        sys.exit("no SVG elements under %s; run ./dev.sh fetch webref" % webref)
    return dict(sorted(spelled.items()))


# The specs whose element and attribute definitions give the spelling of SVG names.
SVG_SPECS = ("SVG2", "SVG11", "filter-effects-1", "svg-animations", "css-masking-1",
             "svg-paths")


def check_svg(webref):
    """Checks that every spelling that webref defines matches the spelling that the parsing table gives."""
    spelled = {}
    for spec in SVG_SPECS:
        path = os.path.join(webref, "ed", "dfns", spec + ".json")
        if not os.path.exists(path):
            continue
        with open(path, encoding="utf-8") as f:
            for dfn in json.load(f)["dfns"]:
                if dfn["type"] == "element-attr":
                    spelled[dfn["linkingText"][0].lower()] = dfn["linkingText"][0]

    checked = 0
    for lowered, cased in ADJUST_SVG.items():
        if lowered not in spelled:
            continue
        checked += 1
        if spelled[lowered] != cased:
            sys.exit("webref spells %s %s, html/#parsing-main-inforeign spells it %s"
                     % (lowered, spelled[lowered], cased))
    return checked


def read(path):
    """Returns every reference, keyed by its name without the '&'. html/#named-character-references lists each
    reference that does not need a semicolon twice, once with the semicolon and once without, so both spellings are
    separate names."""
    with open(path, encoding="utf-8") as f:
        entities = json.load(f)

    names = {name[1:]: entity["characters"] for name, entity in entities.items()}
    return dict(sorted(names.items()))


ENTITIES_BANNER = gen_common.banner(__file__, "https://html.spec.whatwg.org/entities.json")
PARSING_BANNER = gen_common.banner(__file__, "the HTML parsing section, checked against W3C webref",
                                   "https://html.spec.whatwg.org/multipage/parsing.html")


def emit_source(path, names):
    """Writes the table of references and the lookup that pr/html_entity.h declares. The header states the lengths of
    the longest names by hand, and the source asserts that they are the lengths of the names it holds."""
    legacy = [name for name in names if not name.endswith(";")]
    head = [ENTITIES_BANNER, '\n#include "pr/html_entity.h"\n', '\n#include "pr/name_table.h"\n']

    entity = name_table.NameTable("kEntity", list(names), lower=False)
    values = name_table.Blob("kValue", [names[name] for name in names])
    body = ["\nstatic_assert(kPrHtmlEntityMaxLength == %d);\n" % max(len(n) for n in names),
            "static_assert(kPrHtmlEntityMaxLegacyLength == %d);\n" % max(len(n) for n in legacy),
            "\nnamespace {\n", entity.tables(), values.tables(), "\n} // namespace\n"]

    body.append("\nPrStringView prHtmlEntityMatch(PrStringView name) {\n"
                + entity.lookup("name", "index")
                + "    if (!%s)\n        return PrStringView{};\n"
                % entity.confirm("name", entity.name("index"))
                + "    return %s;\n}\n" % values.view("index"))
    write(path, namespaced(head, body, public=True))


def emit_adjust_header(path):
    head = [PARSING_BANNER,
            "\n// Foreign names\n//\n// html/#parsing-main-inforeign - "
            "https://html.spec.whatwg.org/multipage/parsing.html#parsing-main-inforeign\n",
            "\n#pragma once\n", '\n#include "dom/src/dom_node.h"\n']
    body = ["\n// The spelling that html/#parsing-main-inforeign gives an element named `name` in "
            "`space`, or\n// the null view if the name keeps its spelling. Only SVG names change. "
            "`name` is lowercase, as the tokenizer\n// gives it.\n"
            "PrStringView htmlAdjustedTag(DomNamespaceId space, PrStringView name);\n",
            "\n// The same for an attribute named `name` on an element in `space`.\n"
            "PrStringView htmlAdjustedAttribute(DomNamespaceId space, PrStringView name);\n",
            "\n// The namespace that html/#parsing-main-inforeign gives the prefixed attribute `name`, with "
            "its local name\n// written to `local`. PrDomNamespace_None for any other name, which leaves `local` "
            "unchanged.\n"
            "DomNamespaceId htmlForeignAttribute(PrStringView name, PrStringView* local);\n"]
    write(path, namespaced(head, body))


def emit_adjust_source(path, tags):
    head = [PARSING_BANNER, '\n#include "html/src/html_adjust.h"\n',
            '\n#include "pr/name_table.h"\n']

    respelled = [(lowered, cased, "PrDomNamespace_Svg") for lowered, cased in ADJUST_SVG.items()]
    respelled += [(lowered, cased, "PrDomNamespace_MathMl") for lowered,
                  cased in ADJUST_MATHML.items()]

    attribute = name_table.NameTable("kAttribute", [lowered for lowered, _, _ in respelled],
                                     lower=False)
    spelling = name_table.Blob("kSpelling", [cased for _, cased, _ in respelled])
    tag = name_table.NameTable("kTag", list(tags), lower=False)
    tag_spelling = name_table.Blob("kTagSpelling", list(tags.values()))
    prefixed = name_table.NameTable("kPrefixed", list(FOREIGN), lower=False)
    local = name_table.Blob("kLocal", [local for _, local in FOREIGN.values()])

    body = ["namespace {\n",
            attribute.tables(), spelling.tables(),
            table("constexpr DomNamespaceId kSpace[]", [space for _, _, space in respelled]),
            tag.tables(), tag_spelling.tables(), prefixed.tables(), local.tables(),
            table("constexpr DomNamespaceId kPrefixedSpace[]",
                  ["PrDomNamespace_" + space for space, _ in FOREIGN.values()]),
            "\n} // namespace\n"]

    body.append("\nPrStringView htmlAdjustedTag(DomNamespaceId space, PrStringView name) {\n"
                "    if (space != PrDomNamespace_Svg)\n        return PrStringView{};\n"
                + tag.lookup("name", "index")
                + "    if (!%s)\n" % tag.confirm("name", tag.name("index"))
                + "        return PrStringView{};\n"
                + "    return %s;\n}\n" % tag_spelling.view("index"))

    body.append("\nPrStringView htmlAdjustedAttribute(DomNamespaceId space, PrStringView name) {\n"
                + attribute.lookup("name", "index")
                + "    if (space != kSpace[index]\n        || !%s)\n"
                % attribute.confirm("name", attribute.name("index"))
                + "        return PrStringView{};\n"
                + "    return %s;\n}\n" % spelling.view("index"))

    body.append("\nDomNamespaceId htmlForeignAttribute(PrStringView name, PrStringView* local) {\n"
                + prefixed.lookup("name", "index")
                + "    if (!%s)\n" % prefixed.confirm("name", prefixed.name("index"))
                + "        return PrDomNamespace_None;\n"
                + "    *local = %s;\n" % local.view("index")
                + "    return kPrefixedSpace[index];\n}\n")
    write(path, namespaced(head, body))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--entities",
                    default=os.path.join(gen_common.ROOT, "third_party", "html", "entities.json"))
    ap.add_argument("--webref", default=gen_common.WEBREF)
    ap.add_argument("--out", default=os.path.join(gen_common.ROOT, "lib", "html", "src"))
    ap.add_argument("--entity-out", default=os.path.join(gen_common.ROOT, "lib", "html_entity", "src"))
    args = ap.parse_args()

    names = read(args.entities)
    emit_source(os.path.join(args.entity_out, "html_entity.cpp"), names)
    print("%d references, %d of them valid without a semicolon"
          % (len(names), sum(1 for name in names if not name.endswith(";"))))

    tags = adjusted_tags(args.webref)
    checked = check_svg(args.webref)
    emit_adjust_header(os.path.join(args.out, "html_adjust.h"))
    emit_adjust_source(os.path.join(args.out, "html_adjust.cpp"), tags)
    print("%d tags respelled, and %d attributes with %d of them checked against webref"
          % (len(tags), len(ADJUST_SVG) + len(ADJUST_MATHML), checked))



if __name__ == "__main__":
    main()
