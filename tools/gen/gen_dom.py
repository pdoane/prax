#!/usr/bin/env python3
"""Code generator for the DOM tag table and the attribute keywords.

Reads the W3C webref "ed" (editor's drafts) element and definition extracts and emits:

    dom_tag.h/.cpp          an enum of every element name that the engine names, the lookups over it, and the sets
                            of tags that the hand-written TAG_SETS table names
    dom_attribute.h/.cpp    the states of the enumerated attributes that the engine reads, and the names of the
                            attributes whose values selectors compare case-insensitively
    pr/dom_input_type.h     the states of the enumerated attributes that the API also names
    pr/dom_event_handler.h, dom_event_handler.h/.cpp
                            the event handlers of the event handler mixins, from every IDL fragment that declares
                            one, and the type of the events that each handler handles

Usage: python3 -m tools.gen.gen_dom [--webref DIR] [--out DIR]
"""

import argparse
import collections
import glob
import json
import os
import re
import sys
import textwrap

from tools.gen import gen_common
from tools.gen import name_table
from tools.gen.gen_common import namespaced, table, write

BANNER = gen_common.banner(__file__, gen_common.WEBREF_SOURCE, gen_common.webref_url("elements"))

ATTRIBUTE_BANNER = gen_common.banner(__file__, gen_common.WEBREF_SOURCE,
                                     gen_common.webref_url("dfns"),
                                     "the hand-written tables in gen_dom.py")

# The namespace of each extract's elements, by spec shortname. An extract that is not listed here defines elements in
# a namespace that this engine does not parse.
NAMESPACES = {
    "html": "html",
    "html-ruby-extensions": "html",
    "SVG11": "svg",
    "SVG2": "svg",
    "css-masking-1": "svg",
    "filter-effects-1": "svg",
    "svg-animations": "svg",
    "svg-paths": "svg",
    "mathml-core": "mathml",
}

# Names that html/#tree-construction gives rules for, but for which no spec defines an element in that namespace, so
# webref does not extract them. Each is obsolete (html/#obsolete) or belongs to a MathML level that the extracts do
# not include. gen_dom.py fails if one of them appears in webref.
PARSER_ONLY = {
    "applet": ("html", "html/#obsolete"),
    "basefont": ("html", "html/#obsolete"),
    "bgsound": ("html", "html/#obsolete"),
    "big": ("html", "html/#obsolete"),
    "center": ("html", "html/#obsolete"),
    "dir": ("html", "html/#obsolete"),
    "font": ("html", "html/#obsolete"),
    "frame": ("html", "html/#obsolete"),
    "frameset": ("html", "html/#obsolete"),
    "image": ("html", "html/#parsing-main-inbody rewrites it to img"),
    "keygen": ("html", "html/#obsolete"),
    "listing": ("html", "html/#obsolete"),
    "marquee": ("html", "html/#obsolete"),
    "nobr": ("html", "html/#obsolete"),
    "noembed": ("html", "html/#obsolete"),
    "noframes": ("html", "html/#obsolete"),
    "param": ("html", "html/#obsolete"),
    "plaintext": ("html", "html/#obsolete"),
    "strike": ("html", "html/#obsolete"),
    "tt": ("html", "html/#obsolete"),
    "xmp": ("html", "html/#obsolete"),
    "malignmark": ("mathml", "mathml3, named by html/#parsing-main-inforeign"),
    "mglyph": ("mathml", "mathml3, named by html/#parsing-main-inforeign"),
}

# The MathML elements that the engine renders and webref does not list, because webref extracts MathML Core alone. They
# are the presentation elements of MathML 4 that MathML Core leaves out (mathml4/#presm), mlabeledtr from the legacy
# schema of MathML 4 (mathml4/#presm_mtr), and MathML 3's maligngroup. gen_dom.py fails if webref lists one of them.
MATHML_FULL = {
    "maligngroup": "MathML 3's alignment group, which MathML 4 removed and malignmark's parsing keeps",
    "menclose": "mathml4/#presm_menclose",
    "mfenced": "mathml4/#presm_mfenced",
    "mlabeledtr": "mathml4/#presm_mtr",
    "mlongdiv": "mathml4/#presm_mlongdiv",
    "mscarries": "mathml4/#presm_mscarries",
    "mscarry": "mathml4/#presm_mscarry",
    "msgroup": "mathml4/#presm_msgroup",
    "msline": "mathml4/#presm_msline",
    "msrow": "mathml4/#presm_msrow",
    "mstack": "mathml4/#presm_mstack",
}

# SVG Tiny 1.2 elements that the engine renders, which SVG 2 dropped and webref does not extract. gen_dom.py fails if
# one of them appears in webref.
SVG_TINY = {
    "textArea": "SVGTiny12/text.html#TextAreaElement",
    "tbreak": "SVGTiny12/text.html#tbreakElement",
}


# An enumerated attribute (html/#enumerated-attribute) whose states the engine reads: the enum, the key under which
# webref files the attribute's keywords, the attribute's definition, its missing value default, its invalid value
# default, which is the missing value default where it is None, and its empty value default if it has one. A default
# that no keyword names is a separate state, and is the enum's first member. An enum that the API names is public (Pr),
# and is declared in its own public header.
Enumerated = collections.namedtuple("Enumerated", "enum key anchor missing invalid empty")

ENUMERATED = [
    Enumerated("PrDomInputType", "input/type", "attr-input-type", "text", None, None),
    Enumerated("DomButtonType", "button/type", "attr-button-type", "auto", None, None),
    Enumerated("DomDir", "html-global/dir", "attr-dir", "undefined", None, None),
    Enumerated("DomContentEditable", "html-global/contenteditable", "attr-contenteditable", "inherit", None, "true"),
    Enumerated("DomCorsSettings", "img/crossorigin", "cors-settings-attribute", "no-cors", "anonymous", "anonymous"),
]

# The attributes whose values selectors compare ASCII case-insensitively on an HTML element
# (html/#case-sensitivity-of-selectors). HTML lists them in prose, which webref does not extract.
CASE_INSENSITIVE_ATTRIBUTES = [
    "accept", "accept-charset", "align", "alink", "axis", "bgcolor", "charset", "checked",
    "clear", "codetype", "color", "compact", "declare", "defer", "dir", "direction", "disabled",
    "enctype", "face", "frame", "hreflang", "http-equiv", "lang", "language", "link", "media",
    "method", "multiple", "nohref", "noresize", "noshade", "nowrap", "readonly", "rel", "rev",
    "rules", "scope", "scrolling", "selected", "shape", "target", "text", "type", "valign",
    "valuetype", "vlink",
]


# The mixins that DomEventHandlerMixins has a flag for (html/#globaleventhandlers), each with its flag.
HANDLER_MIXINS = {
    "GlobalEventHandlers": "DomEventHandlerMixin_Global",
    "WindowEventHandlers": "DomEventHandlerMixin_Window",
}

# The event handlers whose event type is not their name without "on". HTML gives the type in prose beside each
# handler, as in html/#handler-onwebkitanimationend.
HANDLER_TYPES = {
    "onwebkitanimationend": "webkitAnimationEnd",
    "onwebkitanimationiteration": "webkitAnimationIteration",
    "onwebkitanimationstart": "webkitAnimationStart",
    "onwebkittransitionend": "webkitTransitionEnd",
}

HANDLER_BANNER = gen_common.banner(__file__, gen_common.WEBREF_SOURCE, gen_common.webref_url("idl"),
                                   "the hand-written tables in gen_dom.py")


# The sets of tags that code tests a tag against, each emitted as a bitset over PrDomTag (pr/bitset.h): its name, what
# it holds, and its tags' names. PrDomTag spans every namespace, so a set serves any producer of elements.
TAG_SETS = [
    ("kDomTagsHtmlBoxes", "The HTML elements whose markup decides more of their boxes than their style does, which "
     "HTML's provider answers for (css_boxes.h).",
     ["audio", "bdi", "br", "button", "canvas", "col", "colgroup", "details", "embed", "frame", "frameset", "iframe",
      "img", "input", "meter", "object", "progress", "select", "td", "textarea", "th", "video", "wbr"]),
    ("kDomTagsHtmlParagraphs", "The HTML elements that two line breaks set apart in their rendered text (css_boxes.h).",
     ["p"]),
    ("kDomTagsSvgBoxes", "The SVG elements whose markup decides more of their boxes than their style does, which "
     "SVG's provider answers for (css_boxes.h).",
     ["svg"]),
    ("kDomTagsMathMlBoxes", "The MathML elements whose markup gives their boxes more than their style does, which "
     "MathML's box provider answers for (css_boxes.h).",
     ["mglyph", "mi", "mn", "mo", "ms", "mtd", "mtext"]),
]


def camel(name):
    return "".join(p[:1].upper() + p[1:] for p in name.split("-"))


def tag_ident(name):
    return "PrDomTag_" + camel(name)


def read(webref):
    """Returns every element name, mapped to the namespaces that it is defined in."""
    spaces = collections.defaultdict(set)
    for path in sorted(glob.glob(os.path.join(webref, "ed", "elements", "*.json"))):
        shortname = os.path.basename(path)[:-len(".json")]
        space = NAMESPACES.get(shortname)
        if space is None:
            continue
        with open(path, encoding="utf-8") as f:
            for element in json.load(f)["elements"]:
                spaces[element["name"]].add(space)

    for name, (space, why) in PARSER_ONLY.items():
        if space in spaces[name]:
            sys.exit("webref defines the %s element %s now; drop it from PARSER_ONLY (%s)"
                     % (space, name, why))
        spaces[name].add(space)
    for name, where in MATHML_FULL.items():
        if "mathml" in spaces[name]:
            sys.exit("webref defines the mathml element %s now; drop it from MATHML_FULL (%s)"
                     % (name, where))
        spaces[name].add("mathml")
    for name, where in SVG_TINY.items():
        if "svg" in spaces[name]:
            sys.exit("webref defines the svg element %s now; drop it from SVG_TINY (%s)"
                     % (name, where))
        spaces[name].add("svg")
    return spaces


def check(spaces):
    """Exits if two identifiers collide, because the generated code would not compile."""
    idents = collections.defaultdict(list)
    for name in spaces:
        idents[tag_ident(name)].append(name)
    for ident, names in idents.items():
        if len(names) > 1:
            sys.exit("%s is %s" % (ident, " and ".join(sorted(names))))


def emit_tag_enum(path, names):
    members = [("PrDomTag_Unknown", "")]
    members += [(tag_ident(name), name if tag_ident(name)[len("PrDomTag_"):].lower() != name
                 else "") for name in names]
    head = [BANNER, "\n// Element names\n//\n// dom - https://dom.spec.whatwg.org/\n",
            "\n#pragma once\n", '\n#include "pr/base.h"\n']
    body = ["\nPR_ASSUME_NONNULL_BEGIN\n",
            "\n// Every element name that a spec defines in a namespace the engine parses, and the names that only\n"
            "// html/#tree-construction uses. A name that several namespaces define is one tag. The comment beside a tag\n"
            "// is its name, where the identifier does not spell the name in lowercase.\n"
            "enum {\n"]
    body += gen_common.enum_members(members)
    body.append("\n    PrDomTag_Count,\n};\n")
    body.append("typedef uint16_t PrDomTag;\n")
    body.append("\nPR_ASSUME_NONNULL_END\n")
    write(path, head + body)


def emit_header(path, names):
    head = [BANNER, "\n#pragma once\n", '\n#include "pr/bitset.h"\n#include "pr/dom.h"\n#include "pr/dom_tag.h"\n']
    body = []
    body.append("\n// The tag's name as its defining spec writes it, or the empty name for PrDomTag_Unknown.\n"
                "PrStringView domTagName(PrDomTag tag);\n")
    body.append("\n// The tag's name in ASCII uppercase, as an HTML element's tagName in an HTML "
                "document has it\n// (dom/#element-html-uppercased-qualified-name).\n"
                "PrStringView domTagUpperName(PrDomTag tag);\n")
    body.append("\n// The tag whose name is `text`, or PrDomTag_Unknown. The comparison is case-sensitive, so "
                "`textpath` does\n// not find SVG's `textPath`. A foreign name is respelled by htmlAdjustedTag "
                "(html/#parsing-main-inforeign)\n// before it is looked up.\n"
                "PrDomTag domTagFromText(PrStringView text);\n")
    body.append("\n// The number of words of a set of tags, a bitset over PrDomTag (pr/bitset.h).\n"
                "constexpr size_t kDomTagWords = bitsetWords(PrDomTag_Count);\n")
    for ident, comment, _ in TAG_SETS:
        body.append("\n" + "".join("// %s\n" % line for line in textwrap.wrap(comment, 117, break_on_hyphens=False)))
        body.append("extern const uint64_t %s[kDomTagWords];\n" % ident)
    write(path, namespaced(head, body))


def emit_source(path, names):
    head = [BANNER, '\n#include "dom/src/dom_tag.h"\n', '\n#include "pr/name_table.h"\n']

    table = name_table.NameTable("kDomTag", [""] + names, lower=False)
    upper = name_table.Blob("kDomTagUpper", [name.upper() for name in [""] + names])
    body = ["namespace {\n", table.tables(), upper.tables(), "\n} // namespace\n"]

    body.append("\nPrStringView domTagName(PrDomTag tag) {\n    return %s;\n}\n"
                % table.name("tag"))
    body.append("\nPrStringView domTagUpperName(PrDomTag tag) {\n    return %s;\n}\n"
                % upper.view("tag"))
    body.append("\nPrDomTag domTagFromText(PrStringView text) {\n"
                + table.lookup("text", "tag")
                + "    return %s ? PrDomTag(tag) : PrDomTag_Unknown;\n}\n"
                % table.confirm("text", "domTagName(PrDomTag(tag))"))
    idents = [""] + [tag_ident(name) for name in names]
    for ident, _, members in TAG_SETS:
        unknown = [name for name in members if name not in names]
        if unknown:
            sys.exit("%s names %s, which no tag is" % (ident, ", ".join(unknown)))
        bits = [i > 0 and names[i - 1] in members for i in range(len(names) + 1)]
        body.append(gen_common.bitset("const uint64_t %s[kDomTagWords]" % ident, bits, idents))
    write(path, namespaced(head, body))


def read_keywords(webref):
    """Returns the states of each enumerated attribute in ENUMERATED, in the order that HTML defines them. Each state
    is the keyword that names it, or the empty name for a default that no keyword names."""
    with open(os.path.join(webref, "ed", "dfns", "html.json"), encoding="utf-8") as f:
        dfns = json.load(f)["dfns"]
    states = {}
    for e in ENUMERATED:
        found = [dfn["linkingText"][0] for dfn in dfns if dfn["type"] == "attr-value"
                 and e.key in dfn["for"]]
        if not found:
            sys.exit("webref defines no keywords for %s" % e.key)
        for default, which in ((e.empty, "empty"), (e.invalid, "invalid")):
            if default is not None and default not in found:
                sys.exit("%s has no keyword %s for its %s value default" % (e.key, default, which))
        states[e.key] = found if e.missing in found else [""] + found
    return states


def state_ident(enum, name, default):
    return "%s_%s" % (enum, camel(name or default))


def enum_stem(enum):
    """Returns the enum's name without its prefix, which the names of the functions over it use: InputType for
    PrDomInputType."""
    return enum[len("PrDom"):] if enum.startswith("Pr") else enum[len("Dom"):]


def public_header(enum):
    """Returns the public header of a public enum: pr/dom_input_type.h for PrDomInputType."""
    return "dom_" + re.sub(r"(?<!^)(?=[A-Z])", "_", enum_stem(enum)).lower() + ".h"


def emit_public_enum(path, enum, key, anchor, default, states):
    title = re.sub(r"(?<!^)(?=[A-Z])", " ", enum_stem(enum)).capitalize() + "s"
    head = [ATTRIBUTE_BANNER, "\n// %s\n//\n// html - https://html.spec.whatwg.org/\n" % title,
            "\n#pragma once\n", '\n#include "pr/base.h"\n']
    body = ["\nPR_ASSUME_NONNULL_BEGIN\n",
            "\n// The states of %s (%s), each beside its keyword.\nenum {\n" % (key, "html/#" + anchor)]
    body += gen_common.enum_members([(state_ident(enum, name, default), name) for name in states[key]])
    body.append("\n    %s_Count,\n};\ntypedef uint8_t %s;\n" % (enum, enum))
    body.append("\nPR_ASSUME_NONNULL_END\n")
    write(path, head + body)


def emit_attribute_header(path, states):
    head = [ATTRIBUTE_BANNER,
            "\n// Attribute keywords\n//\n// html - https://html.spec.whatwg.org/\n",
            "\n#pragma once\n", '\n#include "pr/base.h"\n#include "pr/dom.h"\n']
    head += ['#include "pr/%s"\n' % public_header(e.enum) for e in ENUMERATED if e.enum.startswith("Pr")]
    body = []
    for e in ENUMERATED:
        enum, key, anchor, default, empty = e.enum, e.key, e.anchor, e.missing, e.empty
        if not enum.startswith("Pr"):
            body.append("\n// The states of %s (%s).\nenum {\n" % (key, "html/#" + anchor))
            body += gen_common.enum_members([(state_ident(enum, name, default), name)
                                             for name in states[key]])
            body.append("};\ntypedef uint8_t %s;\n" % enum)
        empty_note = ", or %s for the empty string" % state_ident(enum, empty,
                                                                  default) if empty else ""
        if e.invalid is None:
            defaults = ("A missing attribute or any other value gives %s, which is both the missing value default and "
                        "the invalid value default." % state_ident(enum, default, default))
        else:
            defaults = ("A missing attribute gives %s, the missing value default, and any other value gives %s, the "
                        "invalid value default." % (state_ident(enum, default, default),
                                                   state_ident(enum, e.invalid, default)))
        comment = ("The state that the attribute's `value` names, ASCII case-insensitively%s. The null view stands "
                   "for a missing attribute. %s" % (empty_note, defaults))
        body.append("\n" + "".join("// %s\n" % line for line in textwrap.wrap(comment, 96)))
        body.append("%s dom%s(PrStringView value);\n" % (enum, enum_stem(enum)))
        body.append("\n// The keyword of `state`, or the empty string for a default that no keyword "
                    "names.\nPrStringView dom%sKeyword(%s state);\n" % (enum_stem(enum), enum))
    body.append("\n// Whether selectors compare the value of the attribute `name` ASCII "
                "case-insensitively\n// on an HTML element (html/#case-sensitivity-of-selectors). "
                "`name` is in ASCII lowercase.\nbool "
                "domCaseInsensitiveAttribute(PrStringView name);\n")
    write(path, namespaced(head, body))


def emit_attribute_source(path, states):
    head = [ATTRIBUTE_BANNER, '\n#include "dom/src/dom_attribute.h"\n',
            '\n#include "pr/name_table.h"\n#include "dom/src/dom_node.h"\n']
    body = []
    for e in ENUMERATED:
        enum, key, default, empty = e.enum, e.key, e.missing, e.empty
        # A default that no keyword names is the empty name of entry zero. Only the empty string reaches it, and the
        # empty string is an invalid value unless the attribute has an empty value default.
        table = name_table.NameTable("k%s" % enum_stem(enum), states[key], lower=True)
        body += ["\nnamespace {\n", table.tables(), "\n} // namespace\n"]
        missing = state_ident(enum, default, default)
        fallback = state_ident(enum, e.invalid, default) if e.invalid is not None else missing
        lines = ("\n%s dom%s(PrStringView value) {\n"
                 "    if (value.data == nullptr)\n        return %s;\n"
                 % (enum, enum_stem(enum), missing))
        if empty is not None:
            lines += "    if (value.size == 0)\n        return %s;\n" % state_ident(enum, empty,
                                                                                    default)
        lines += table.lookup("value", "state")
        lines += "    return %s ? %s(state) : %s;\n}\n" % (table.confirm("value",
                                                                         table.name("state")), enum,
                                                                         fallback)
        lines += ("\nPrStringView dom%sKeyword(%s state) {\n    return %s;\n}\n"
                  % (enum_stem(enum), enum, table.name("state")))
        body.append(lines)

    names = name_table.NameTable("kCaseInsensitiveAttribute", CASE_INSENSITIVE_ATTRIBUTES,
                                 lower=False)
    body += ["\nnamespace {\n", names.tables(), "\n} // namespace\n"]
    body.append("\nbool domCaseInsensitiveAttribute(PrStringView name) {\n"
                + names.lookup("name", "index")
                + "    return %s;\n}\n" % names.confirm("name", names.name("index")))
    write(path, namespaced(head, body))


def read_handlers(webref):
    """Returns the event handler IDL attributes of every interface and mixin, each with the mixins in HANDLER_MIXINS
    that declare it."""
    pattern = re.compile(r"(?:partial\s+)?interface(\s+mixin)?\s+(\w+)[^{]*\{(.*?)\};", re.S)
    names = collections.defaultdict(set)
    for path in sorted(glob.glob(os.path.join(webref, "ed", "idl", "*.idl"))):
        with open(path, encoding="utf-8") as f:
            text = f.read()
        for match in pattern.finditer(text):
            for name in re.findall(r"attribute\s+\w*EventHandler\s+(on\w+)\s*;", match.group(3)):
                names[name]
                if match.group(1) and match.group(2) in HANDLER_MIXINS:
                    names[name].add(HANDLER_MIXINS[match.group(2)])
    if not any(names.values()):
        sys.exit("webref declares no event handlers of the mixins")
    for name in HANDLER_TYPES:
        if not names.get(name):
            sys.exit("no mixin has %s now; drop it from HANDLER_TYPES" % name)
    return names


def emit_handler_enum(path, handlers):
    head = [HANDLER_BANNER,
            "\n// Event handlers\n//\n// html - "
            "https://html.spec.whatwg.org/multipage/webappapis.html#event-handler-attributes\n",
            "\n#pragma once\n", '\n#include "pr/base.h"\n']
    body = ["\nPR_ASSUME_NONNULL_BEGIN\n",
            "\n// Every event handler IDL attribute of an interface or a mixin "
            "(html/#event-handler-idl-attributes). Each\n// enumerator ends with the attribute's name.\nenum {\n"]
    body += ["    PrDomEventHandler_%s,\n" % name for name in sorted(handlers)]
    body.append("\n    PrDomEventHandler_Count,\n};\ntypedef uint16_t PrDomEventHandler;\n")
    body.append("\nPR_ASSUME_NONNULL_END\n")
    write(path, head + body)


def emit_handler_header(path):
    head = [HANDLER_BANNER, "\n#pragma once\n", '\n#include "pr/dom.h"\n#include "pr/dom_event_handler.h"\n']
    body = ["\n// The event handler mixins, as flags (html/#globaleventhandlers).\nenum {\n"]
    body += gen_common.enum_members([(ident + " = 1 << %d" % i, mixin) for i,
                                     (mixin, ident) in enumerate(HANDLER_MIXINS.items())])
    body.append("};\ntypedef uint8_t DomEventHandlerMixins;\n")
    body.append("\n// The type of the events that `handler` handles. It is the handler's name after \"on\", "
                "except for the\n// legacy handlers whose type HTML spells differently.\n"
                "PrStringView domEventHandlerType(PrDomEventHandler handler);\n")
    body.append("\n// The mixins that declare `handler`.\n"
                "DomEventHandlerMixins domEventHandlerMixins(PrDomEventHandler handler);\n")
    body.append("\n// The event handler named `name`, or PrDomEventHandler_Count for a name that no "
                "interface declares.\nPrDomEventHandler domEventHandlerFromName(PrStringView name);\n")
    write(path, namespaced(head, body))


def emit_handler_source(path, handlers):
    names = sorted(handlers)
    types = [HANDLER_TYPES.get(name, name[2:]) for name in names]
    blob = name_table.Blob("kDomEventHandlerType", types)
    lookup = name_table.NameTable("kDomEventHandlerName", names, lower=False)
    mixins = [" | ".join(sorted(handlers[name])) or "0" for name in names]
    head = [HANDLER_BANNER, '\n#include "dom/src/dom_event_handler.h"\n',
            '\n#include "pr/name_table.h"\n']
    body = ["namespace {\n", blob.tables(), lookup.tables(),
            table("constexpr DomEventHandlerMixins kDomEventHandlerMixins[]", mixins, 1, names),
            "\n} // namespace\n",
            "\nPrStringView domEventHandlerType(PrDomEventHandler handler) {\n    return %s;\n}\n"
            % blob.view("handler"),
            "\nDomEventHandlerMixins domEventHandlerMixins(PrDomEventHandler handler) {\n"
            "    return kDomEventHandlerMixins[handler];\n}\n",
            "\nPrDomEventHandler domEventHandlerFromName(PrStringView name) {\n"
            + lookup.lookup("name", "handler")
            + "    return %s ? PrDomEventHandler(handler) : PrDomEventHandler_Count;\n}\n"
            % lookup.confirm("name", lookup.name("handler"))]
    write(path, namespaced(head, body))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--webref", default=gen_common.WEBREF)
    ap.add_argument("--out", default=os.path.join(gen_common.ROOT, "lib", "dom", "src"))
    args = ap.parse_args()

    spaces = read(args.webref)
    check(spaces)
    names = sorted(spaces, key=lambda n: (n.lower(), n))

    emit_tag_enum(os.path.join(gen_common.ROOT, "lib", "dom", "include", "pr", "dom_tag.h"), names)
    emit_header(os.path.join(args.out, "dom_tag.h"), names)
    emit_source(os.path.join(args.out, "dom_tag.cpp"), names)

    states = read_keywords(args.webref)
    for e in ENUMERATED:
        if e.enum.startswith("Pr"):
            emit_public_enum(os.path.join(gen_common.ROOT, "lib", "dom", "include", "pr", public_header(e.enum)),
                             e.enum, e.key, e.anchor, e.missing, states)
    emit_attribute_header(os.path.join(args.out, "dom_attribute.h"), states)
    emit_attribute_source(os.path.join(args.out, "dom_attribute.cpp"), states)

    handlers = read_handlers(args.webref)
    emit_handler_enum(os.path.join(gen_common.ROOT, "lib", "dom", "include", "pr", "dom_event_handler.h"),
                      handlers)
    emit_handler_header(os.path.join(args.out, "dom_event_handler.h"))
    emit_handler_source(os.path.join(args.out, "dom_event_handler.cpp"), handlers)

    counts = collections.Counter(space for name in names for space in spaces[name])
    print("%d tags: %s" % (len(names), ", ".join("%d %s" % (counts[s], s) for s in sorted(counts))))


if __name__ == "__main__":
    main()
