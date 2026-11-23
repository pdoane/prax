#!/usr/bin/env python3
"""Generates SVG's tables of attributes.

    pr/svg_attribute.h          PrSvgAttribute, the attributes that lib/svg reads as values or animates, and the
                                enumerations of their keywords
    svg_attribute.h/.cpp        what each element reads each typed attribute as, and the lookups of the keywords
    svg_animate_names.h/.cpp    the attributes whose values interpolate, with the type that each animates as, the axis
                                that a percentage in them measures and the value that the attribute has where an
                                element does not set it
"""

import os
import re
import sys

from tools.gen.gen_common import ROOT, banner, enum_members, table, write
from tools.gen.name_table import Blob, NameTable

# The attributes of SVG2/attindex.html whose values interpolate, with their animation types
# (svg-animations/#Animatable). An attribute that is not here animates discretely. None is no initial value.
ATTRIBUTES = [
    ("amplitude", "Number", "Other", None),
    ("azimuth", "Number", "Other", None),
    ("baseFrequency", "NumberOptionalNumber", "Other", None),
    ("bias", "Number", "Other", None),
    ("color", "Color", "Other", "black"),
    ("cx", "Length", "X", "0"),
    ("cy", "Length", "Y", "0"),
    ("d", "Path", "Other", None),
    ("diffuseConstant", "Number", "Other", None),
    ("divisor", "Number", "Other", None),
    ("dx", "LengthList", "X", None),
    ("dy", "LengthList", "Y", None),
    ("elevation", "Number", "Other", None),
    ("exponent", "Number", "Other", None),
    ("fill", "Color", "Other", "black"),
    ("fill-opacity", "Number", "Other", "1"),
    ("flood-color", "Color", "Other", "black"),
    ("flood-opacity", "Number", "Other", "1"),
    ("font-size", "Length", "Other", None),
    ("fr", "Length", "Other", "0"),
    ("fx", "Length", "X", None),
    ("fy", "Length", "Y", None),
    ("height", "Length", "Y", None),
    ("intercept", "Number", "Other", None),
    ("k1", "Number", "Other", None),
    ("k2", "Number", "Other", None),
    ("k3", "Number", "Other", None),
    ("k4", "Number", "Other", None),
    ("kernelMatrix", "NumberList", "Other", None),
    ("kernelUnitLength", "NumberOptionalNumber", "Other", None),
    ("letter-spacing", "Length", "Other", None),
    ("lighting-color", "Color", "Other", "white"),
    ("limitingConeAngle", "Number", "Other", None),
    ("markerHeight", "Length", "Y", "3"),
    ("markerWidth", "Length", "X", "3"),
    ("offset", "Number", "Other", "0"),
    ("opacity", "Number", "Other", "1"),
    ("order", "IntegerOptionalInteger", "Other", None),
    ("orient", "Angle", "Other", "0"),
    ("pathLength", "Number", "Other", "0"),
    ("points", "PointList", "Other", None),
    ("pointsAtX", "Number", "X", None),
    ("pointsAtY", "Number", "Y", None),
    ("pointsAtZ", "Number", "Other", None),
    ("r", "Length", "Other", "0"),
    ("radius", "NumberOptionalNumber", "Other", None),
    ("refX", "Length", "X", "0"),
    ("refY", "Length", "Y", "0"),
    ("rotate", "NumberList", "Other", None),
    ("rx", "Length", "X", None),
    ("ry", "Length", "Y", None),
    ("scale", "Number", "Other", None),
    ("seed", "Number", "Other", None),
    ("slope", "Number", "Other", None),
    ("specularConstant", "Number", "Other", None),
    ("specularExponent", "Number", "Other", None),
    ("startOffset", "Length", "X", "0"),
    ("stdDeviation", "NumberOptionalNumber", "Other", None),
    ("stop-color", "Color", "Other", "black"),
    ("stop-opacity", "Number", "Other", "1"),
    ("stroke", "Color", "Other", "none"),
    ("stroke-dasharray", "LengthList", "Other", None),
    ("stroke-dashoffset", "Length", "Other", "0"),
    ("stroke-miterlimit", "Number", "Other", "4"),
    ("stroke-opacity", "Number", "Other", "1"),
    ("stroke-width", "Length", "Other", "1"),
    ("surfaceScale", "Number", "Other", None),
    ("tableValues", "NumberList", "Other", None),
    ("targetX", "Integer", "X", None),
    ("targetY", "Integer", "Y", None),
    ("textLength", "Length", "X", "0"),
    ("viewBox", "Rect", "Other", None),
    ("width", "Length", "X", None),
    ("word-spacing", "Length", "Other", None),
    ("x", "LengthList", "X", "0"),
    ("x1", "Length", "X", "0"),
    ("x2", "Length", "X", "0"),
    ("y", "LengthList", "Y", "0"),
    ("y1", "Length", "Y", "0"),
    ("y2", "Length", "Y", "0"),
    ("z", "Number", "Other", None),
]

# The PrSvgValueType that the values of each type of attribute animate as (SVG2/attindex.html). An attribute that
# animates discretely, as one that the index does not list does, animates as its text: a string.
TYPES = {
    "Number": "PrSvgValueType_Number", "Integer": "PrSvgValueType_Integer", "Length": "PrSvgValueType_Length",
    "LengthList": "PrSvgValueType_LengthList", "NumberList": "PrSvgValueType_NumberList",
    "NumberOptionalNumber": "PrSvgValueType_NumberPair", "IntegerOptionalInteger": "PrSvgValueType_IntegerPair",
    "PointList": "PrSvgValueType_PointList", "Angle": "PrSvgValueType_Orient", "Rect": "PrSvgValueType_Rect",
    "Color": "PrSvgValueType_Color", "Path": "PrSvgValueType_Path",
}


# ----------------------------------------------------------------------------------------------------------------------
# The typed attributes of SVG: the attributes whose values lib/svg reads as one of its types (pr/svg_types.h), keyed by
# the element's tag and the attribute, and the enumerations of their keywords.

# The elements of each group that shares attributes.
GRAPHICS = ["a", "circle", "defs", "ellipse", "foreignObject", "g", "image", "line", "path", "polygon", "polyline",
            "rect", "svg", "switch", "symbol", "text", "textPath", "tspan", "use"]
SHAPES = ["circle", "ellipse", "line", "path", "polygon", "polyline", "rect"]
TEXT_POSITIONING = ["text", "tspan"]
TEXT_CONTENT = ["text", "textPath", "tspan"]
FIT_TO_VIEW_BOX = ["marker", "pattern", "svg", "symbol", "view"]
URI_REFERENCE = ["a", "feImage", "image", "linearGradient", "mpath", "pattern", "radialGradient", "script", "textPath",
                 "use"]
GRADIENTS = ["linearGradient", "radialGradient"]
PRIMITIVES = ["feBlend", "feColorMatrix", "feComponentTransfer", "feComposite", "feConvolveMatrix", "feDiffuseLighting",
              "feDisplacementMap", "feDropShadow", "feFlood", "feGaussianBlur", "feImage", "feMerge", "feMorphology",
              "feOffset", "feSpecularLighting", "feTile", "feTurbulence"]
FUNCTIONS = ["feFuncA", "feFuncB", "feFuncG", "feFuncR"]

# Each enumeration of keywords: its C type, and its keywords, by value from 1 as the constants of the IDL number them.
# An empty keyword is a value that no keyword spells, such as a marker's orient of an angle, with its identifier.
ENUMERATIONS = {
    "units": ("PrSvgUnits", ["userSpaceOnUse", "objectBoundingBox"]),
    "spreadMethod": ("PrSvgSpreadMethod", ["pad", "reflect", "repeat"]),
    "lengthAdjust": ("PrSvgLengthAdjust", ["spacing", "spacingAndGlyphs"]),
    "method": ("PrSvgTextPathMethod", ["align", "stretch"]),
    "spacing": ("PrSvgTextPathSpacing", ["auto", "exact"]),
    "side": ("PrSvgTextPathSide", ["left", "right"]),
    "markerUnits": ("PrSvgMarkerUnits", ["userSpaceOnUse", "strokeWidth"]),
    "orient": ("PrSvgOrient", ["auto", ("", "Angle"), "auto-start-reverse"]),
    "mode": ("PrSvgBlendMode", ["normal", "multiply", "screen", "darken", "lighten", "overlay", "color-dodge",
                                "color-burn", "hard-light", "soft-light", "difference", "exclusion", "hue",
                                "saturation", "color", "luminosity"]),
    "colorMatrixType": ("PrSvgColorMatrixType", ["matrix", "saturate", "hueRotate", "luminanceToAlpha"]),
    "transferType": ("PrSvgTransferType", ["identity", "table", "discrete", "linear", "gamma"]),
    # mirror is Filter Effects 2's, which SVGFEConvolveMatrixElement has no constant for.
    "compositeOperator": ("PrSvgCompositeOperator", ["over", "in", "out", "atop", "xor", "arithmetic", "lighter"]),
    "edgeMode": ("PrSvgEdgeMode", ["duplicate", "wrap", "none", "mirror"]),
    "channel": ("PrSvgChannel", ["R", "G", "B", "A"]),
    "morphologyOperator": ("PrSvgMorphologyOperator", ["erode", "dilate"]),
    "stitchTiles": ("PrSvgStitchTiles", ["stitch", "noStitch"]),
    "turbulenceType": ("PrSvgTurbulenceType", ["fractalNoise", "turbulence"]),
    # The parts of preserveAspectRatio and the functions of a transform list (SVG2/coords.html).
    "align": ("PrSvgAlign", ["none", "xMinYMin", "xMidYMin", "xMaxYMin", "xMinYMid", "xMidYMid", "xMaxYMid",
                             "xMinYMax", "xMidYMax", "xMaxYMax"]),
    "meetOrSlice": ("PrSvgMeetOrSlice", ["meet", "slice"]),
    "transformType": ("PrSvgTransformType", ["matrix", "translate", "scale", "rotate", "skewX", "skewY"]),
}


# The users of the enumerations that no typed attribute is of alone.
USERS = {"align": "the alignment of preserveAspectRatio", "meetOrSlice": "the meet or slice of preserveAspectRatio",
         "transformType": "the functions of a transform list"}


def keyword_of(entry):
    """Returns the spelling of an enumeration's entry, and its identifier."""
    if isinstance(entry, tuple):
        return entry
    return entry, "".join(part[:1].upper() + part[1:] for part in entry.split("-"))


# Each typed attribute: the elements, the attribute, its type, the value of a missing or invalid attribute (None for
# no value), the axis of a length's percentages, an enumeration's keywords, and the range of its valid values.
def typed(tags, attribute, kind, initial=None, axis="Other", keywords=None, range="Any"):
    return [(tag, attribute, kind, initial, axis, keywords, range) for tag in tags]

# The attributes whose negative values are in error, which read as their initial values (SVG2/geometry.html#Sizing,
# SVG2/shapes.html#RectElement, SVG2/painting.html#MarkerWidthAttribute).
NON_NEGATIVE = {"width", "height", "r", "rx", "ry", "fr", "markerWidth", "markerHeight", "textLength", "pathLength"}
X_AXIS = {"x", "width", "x1", "x2", "cx", "fx", "rx", "refX", "markerWidth", "startOffset", "textLength", "dx"}
Y_AXIS = {"y", "height", "y1", "y2", "cy", "fy", "ry", "refY", "markerHeight", "dy"}


def axis_of(attribute):
    return "X" if attribute in X_AXIS else "Y" if attribute in Y_AXIS else "Other"


def lengths(tags, attributes, initials):
    out = []
    for attribute, initial in zip(attributes, initials):
        out += typed(tags, attribute, "Length", initial=initial, axis=axis_of(attribute),
                     range="NonNegative" if attribute in NON_NEGATIVE else "Any")
    return out

TYPED = (
    typed(GRAPHICS + ["clipPath"], "transform", "TransformList") +
    typed(SHAPES, "pathLength", "Number", initial="0", range="NonNegative") +
    typed(["polygon", "polyline"], "points", "PointList") +
    typed(FIT_TO_VIEW_BOX, "viewBox", "Rect") +
    typed(FIT_TO_VIEW_BOX + ["image", "feImage"], "preserveAspectRatio", "PreserveAspectRatio",
          initial="xMidYMid meet") +
    typed(URI_REFERENCE, "href", "String") +
    lengths(["svg"], ["x", "y", "width", "height"], ["0", "0", "100%", "100%"]) +
    lengths(["use", "image", "foreignObject", "pattern"], ["x", "y", "width", "height"], ["0", "0", "0", "0"]) +
    lengths(["rect"], ["x", "y", "width", "height", "rx", "ry"], ["0"] * 6) +
    lengths(["circle"], ["cx", "cy", "r"], ["0"] * 3) +
    lengths(["ellipse"], ["cx", "cy", "rx", "ry"], ["0"] * 4) +
    lengths(["line"], ["x1", "y1", "x2", "y2"], ["0"] * 4) +
    lengths(TEXT_CONTENT, ["textLength"], ["0"]) +
    typed(TEXT_CONTENT, "lengthAdjust", "Enumeration", initial="spacing", keywords="lengthAdjust") +
    typed(TEXT_POSITIONING, "x", "LengthList", axis="X") +
    typed(TEXT_POSITIONING, "y", "LengthList", axis="Y") +
    typed(TEXT_POSITIONING, "dx", "LengthList", axis="X") +
    typed(TEXT_POSITIONING, "dy", "LengthList", axis="Y") +
    typed(TEXT_POSITIONING, "rotate", "NumberList") +
    lengths(["textPath"], ["startOffset"], ["0"]) +
    typed(["textPath"], "method", "Enumeration", initial="align", keywords="method") +
    typed(["textPath"], "spacing", "Enumeration", initial="exact", keywords="spacing") +
    typed(["textPath"], "side", "Enumeration", initial="left", keywords="side") +
    lengths(["marker"], ["refX", "refY", "markerWidth", "markerHeight"], ["0", "0", "3", "3"]) +
    typed(["marker"], "markerUnits", "Enumeration", initial="strokeWidth", keywords="markerUnits") +
    typed(["marker"], "orient", "Orient", initial="0", keywords="orient") +
    typed(GRADIENTS, "gradientUnits", "Enumeration", initial="objectBoundingBox", keywords="units") +
    typed(GRADIENTS, "gradientTransform", "TransformList") +
    typed(GRADIENTS, "spreadMethod", "Enumeration", initial="pad", keywords="spreadMethod") +
    lengths(["linearGradient"], ["x1", "y1", "x2", "y2"], ["0%", "0%", "100%", "0%"]) +
    lengths(["radialGradient"], ["cx", "cy", "r", "fx", "fy", "fr"], ["50%", "50%", "50%", "50%", "50%", "0%"]) +
    typed(["stop"], "offset", "Number", initial="0") +
    typed(["pattern"], "patternUnits", "Enumeration", initial="objectBoundingBox", keywords="units") +
    typed(["pattern"], "patternContentUnits", "Enumeration", initial="userSpaceOnUse", keywords="units") +
    typed(["pattern"], "patternTransform", "TransformList") +
    typed(["a"], "target", "String") +
    typed(["filter"], "filterUnits", "Enumeration", initial="objectBoundingBox", keywords="units") +
    typed(["filter"], "primitiveUnits", "Enumeration", initial="userSpaceOnUse", keywords="units") +
    lengths(["filter", "mask"], ["x", "y", "width", "height"], ["-10%", "-10%", "120%", "120%"]) +
    lengths(PRIMITIVES, ["x", "y", "width", "height"], ["0%", "0%", "100%", "100%"]) +
    typed(PRIMITIVES, "result", "String") +
    typed([t for t in PRIMITIVES if t not in ("feFlood", "feImage", "feMerge", "feTurbulence")] + ["feMergeNode"],
          "in", "String") +
    typed(["feBlend", "feComposite", "feDisplacementMap"], "in2", "String") +
    typed(["feBlend"], "mode", "Enumeration", initial="normal", keywords="mode") +
    typed(["feColorMatrix"], "type", "Enumeration", initial="matrix", keywords="colorMatrixType") +
    typed(["feColorMatrix"], "values", "NumberList") +
    typed(FUNCTIONS, "type", "Enumeration", initial="identity", keywords="transferType") +
    typed(FUNCTIONS, "tableValues", "NumberList") +
    typed(FUNCTIONS, "slope", "Number", initial="1") + typed(FUNCTIONS, "intercept", "Number", initial="0") +
    typed(FUNCTIONS, "amplitude", "Number", initial="1") + typed(FUNCTIONS, "exponent", "Number", initial="1") +
    typed(FUNCTIONS, "offset", "Number", initial="0") +
    typed(["feComposite"], "operator", "Enumeration", initial="over", keywords="compositeOperator") +
    typed(["feComposite"], "k1", "Number", initial="0") + typed(["feComposite"], "k2", "Number", initial="0") +
    typed(["feComposite"], "k3", "Number", initial="0") + typed(["feComposite"], "k4", "Number", initial="0") +
    typed(["feConvolveMatrix"], "order", "IntegerPair", initial="3", range="Positive") +
    typed(["feConvolveMatrix"], "kernelMatrix", "NumberList") +
    typed(["feConvolveMatrix"], "divisor", "Number", initial="1") +
    typed(["feConvolveMatrix"], "bias", "Number", initial="0") +
    typed(["feConvolveMatrix"], "targetX", "Integer", initial="0") +
    typed(["feConvolveMatrix"], "targetY", "Integer", initial="0") +
    typed(["feConvolveMatrix"], "edgeMode", "Enumeration", initial="duplicate", keywords="edgeMode") +
    typed(["feConvolveMatrix"], "preserveAlpha", "Boolean", initial="false") +
    typed(["feConvolveMatrix", "feDiffuseLighting", "feSpecularLighting"], "kernelUnitLength", "NumberPair",
          initial="0") +
    typed(["feDiffuseLighting", "feSpecularLighting"], "surfaceScale", "Number", initial="1") +
    typed(["feDiffuseLighting"], "diffuseConstant", "Number", initial="1") +
    typed(["feSpecularLighting"], "specularConstant", "Number", initial="1") +
    typed(["feSpecularLighting", "feSpotLight"], "specularExponent", "Number", initial="1") +
    typed(["feDistantLight"], "azimuth", "Number", initial="0") +
    typed(["feDistantLight"], "elevation", "Number", initial="0") +
    typed(["fePointLight", "feSpotLight"], "x", "Number", initial="0") +
    typed(["fePointLight", "feSpotLight"], "y", "Number", initial="0") +
    typed(["fePointLight", "feSpotLight"], "z", "Number", initial="0") +
    typed(["feSpotLight"], "pointsAtX", "Number", initial="0") +
    typed(["feSpotLight"], "pointsAtY", "Number", initial="0") +
    typed(["feSpotLight"], "pointsAtZ", "Number", initial="0") +
    typed(["feSpotLight"], "limitingConeAngle", "Number", initial="0") +
    typed(["feDisplacementMap"], "scale", "Number", initial="0") +
    typed(["feDisplacementMap"], "xChannelSelector", "Enumeration", initial="A", keywords="channel") +
    typed(["feDisplacementMap"], "yChannelSelector", "Enumeration", initial="A", keywords="channel") +
    typed(["feDropShadow"], "dx", "Number", initial="2") + typed(["feDropShadow"], "dy", "Number", initial="2") +
    typed(["feOffset"], "dx", "Number", initial="0") + typed(["feOffset"], "dy", "Number", initial="0") +
    typed(["feDropShadow"], "stdDeviation", "NumberPair", initial="2") +
    typed(["feGaussianBlur"], "stdDeviation", "NumberPair", initial="0") +
    typed(["feGaussianBlur"], "edgeMode", "Enumeration", initial="none", keywords="edgeMode") +
    typed(["feImage"], "crossorigin", "String") +
    typed(["feMorphology"], "operator", "Enumeration", initial="erode", keywords="morphologyOperator") +
    typed(["feMorphology"], "radius", "NumberPair", initial="0") +
    typed(["feTurbulence"], "baseFrequency", "NumberPair", initial="0") +
    typed(["feTurbulence"], "numOctaves", "Integer", initial="1") +
    typed(["feTurbulence"], "seed", "Number", initial="0") +
    typed(["feTurbulence"], "stitchTiles", "Enumeration", initial="noStitch", keywords="stitchTiles") +
    typed(["feTurbulence"], "type", "Enumeration", initial="turbulence", keywords="turbulenceType") +
    typed(["clipPath"], "clipPathUnits", "Enumeration", initial="userSpaceOnUse", keywords="units") +
    typed(["mask"], "maskUnits", "Enumeration", initial="objectBoundingBox", keywords="units") +
    typed(["mask"], "maskContentUnits", "Enumeration", initial="userSpaceOnUse", keywords="units")
)

# The typed attributes of every element in the SVG namespace (SVG2/types.html#InterfaceSVGElement,
# SVG2/struct.html#InterfaceSVGTests).
COMMON = [("class", "String"), ("requiredExtensions", "StringList"), ("systemLanguage", "StringList")]


def attribute_ident(name):
    """Returns the identifier of an attribute's PrSvgAttribute."""
    return "".join(part[:1].upper() + part[1:] for part in name.split("-"))


def attribute_names():
    """Returns the attributes of PrSvgAttribute, sorted: those that are typed and those that animations interpolate."""
    names = {row[1] for row in TYPED} | {name for name, _ in COMMON} | {row[0] for row in ATTRIBUTES}
    return sorted(names, key=lambda name: (attribute_ident(name).lower(), name))


def tag_numbers():
    """Returns the value of each tag of pr/dom_tag.h, by its element name."""
    numbers = {}
    with open(os.path.join(ROOT, "lib/dom/include/pr/dom_tag.h"), encoding="utf-8") as f:
        index = 0
        for line in f:
            match = re.match(r"\s+PrDomTag_(\w+),\s*(?://\s*(\S+))?", line)
            if not match:
                continue
            numbers[match.group(2) or match.group(1).lower()] = (index, "PrDomTag_" + match.group(1))
            index += 1
    return numbers


def emit_attributes(source, include, generated):
    tags = tag_numbers()
    names = attribute_names()
    number = {name: index + 1 for index, name in enumerate(names)}
    keywords = list(ENUMERATIONS)

    # The public header: the attributes and the enumerations of keywords.
    enums = []
    for name in keywords:
        ctype, entries = ENUMERATIONS[name]
        members = []
        for index, entry in enumerate(entries):
            spelling, ident = keyword_of(entry)
            member = "%s_%s" % (ctype, ident) + (" = 1" if index == 0 else "")
            members.append((member, spelling if spelling and spelling.lower() != ident.lower() else ""))
        # An attribute that elements read with several enumerations is named with its elements.
        users = []
        for attribute in sorted({row[1] for row in TYPED if row[5] == name}):
            shared = len({row[5] for row in TYPED if row[1] == attribute}) > 1
            elements = sorted({row[0] for row in TYPED if row[1] == attribute and row[5] == name})
            users.append(attribute + (" of " + " and ".join(elements) if shared else ""))
        users = users or [USERS[name]]
        enums += ["\n// The keywords of %s.\nenum {\n" % ", ".join(users)] + enum_members(members) + [
            "};\ntypedef uint8_t %s;\n" % ctype]
    attribute_members = [("PrSvgAttribute_Unknown", "")] + [
        ("PrSvgAttribute_" + attribute_ident(name), name if attribute_ident(name).lower() != name else "")
        for name in names]
    public = [generated,
              "\n// The attributes of SVG that lib/svg reads as values, and the keywords of their enumerations\n"
              "//\n// SVG2 - https://svgwg.org/svg2-draft/\n// filter-effects-1 - https://drafts.fxtf.org/filter-effects-1/\n",
              '\n#pragma once\n\n#include "pr/base.h"\n', "\nPR_ASSUME_NONNULL_BEGIN\n",
              "\n#if defined(__cplusplus)\nextern \"C\" {\n#endif\n",
              "\n// The attributes, in no namespace, that SVG elements read as values (pr/svg_types.h), and the attributes "
              "whose values SVG's animations interpolate. The comment beside an attribute is its name, where the "
              "identifier does not spell the name in lowercase.\nenum {\n"] + enum_members(attribute_members) + [
              "    PrSvgAttribute_Count,\n};\ntypedef uint16_t PrSvgAttribute;\n",
              "\n// Returns the name of the attribute, or the null view for Unknown.\n"
              "PrStringView prSvgAttributeName(PrSvgAttribute attribute);\n",
              "\n// Returns the attribute that `name` names, or Unknown if it names none.\n"
              "PrSvgAttribute prSvgAttributeFromName(PrStringView name);\n",
              "\n// The keywords of the enumerations that typed attributes read as, each numbered from 1 as the constants "
              "of its interface number it (SVG2/types.html#InterfaceSVGAnimatedEnumeration). An entry without a comment "
              "is the keyword that its identifier spells. A marker's orient of an angle has no keyword.\n"] + enums + [
              "\n#if defined(__cplusplus)\n}\n#endif\n\nPR_ASSUME_NONNULL_END\n"]
    write(os.path.join(include, "svg_attribute.h"), public)

    # The internal header and source: what each typed attribute reads as, and the keywords' lookups.
    rows = sorted(TYPED, key=lambda row: (tags[row[0]][0], number[row[1]]))
    keys = [(tags[row[0]][0] << 16) | number[row[1]] for row in rows]
    if len(set(keys)) != len(keys):
        sys.exit("gen_svg: an element has an attribute twice")
    spelled = [(name, entry) for name in keywords for entry in ENUMERATIONS[name][1] if keyword_of(entry)[0] != ""]
    words = [""] + [keyword_of(entry)[0] for _, entry in spelled]
    word_salts = [0] + [keywords.index(name) + 1 for name, _ in spelled]
    word_values = [0] + [ENUMERATIONS[name][1].index(entry) + 1 for name, entry in spelled]
    word_table = NameTable("kSvgKeywordWord", words, lower=False, salts=word_salts)
    word_blob = Blob("kSvgKeywordName", [""] + [keyword_of(entry)[0] for name in keywords
                                                for entry in ENUMERATIONS[name][1]])
    starts = [0]
    at = 1
    for name in keywords:
        starts.append(at)
        at += len(ENUMERATIONS[name][1])
    starts.append(at)
    attribute_table = NameTable("kSvgAttributeName", [""] + names, lower=False)
    initials = Blob("kSvgTypedInitial", [row[3] or "" for row in rows])
    common = [("PrSvgAttribute_" + attribute_ident(name), "PrSvgValueType_" + kind) for name, kind in COMMON]

    header = [generated, '\n#pragma once\n\n#include "pr/base.h"\n#include "pr/dom_tag.h"\n#include "pr/svg_types.h"\n',
              '\n#include "svg/src/svg_style.h"\n', "\nPR_ASSUME_NONNULL_BEGIN\n\nnamespace pr_impl {\n",
              "\n// The enumerations of keywords that typed attributes read as.\nenum {\n    SvgKeywords_None,\n",
              "".join("    SvgKeywords_%s,\n" % (name[0].upper() + name[1:]) for name in keywords),
              "};\ntypedef uint8_t SvgKeywords;\n",
              "\n// The values of an attribute that are not in error. A value in error reads as the attribute's "
              "initial value. An integer is truncated toward zero before it is checked.\nenum {\n"
              "    SvgRange_Any,\n    SvgRange_NonNegative,     // zero or greater\n"
              "    SvgRange_Positive,        // greater than zero\n};\ntypedef uint8_t SvgRange;\n",
              "\n// What an attribute of an element reads as: its type, its value where it is missing or does not parse "
              "(the null view for none), the axis of a length's percentages, an enumeration's keywords, and the range "
              "of its values that are not in error.\n",
              "struct SvgTypedAttribute {\n    PrStringView initial;\n    PrSvgValueType type;\n    SvgAxis axis;\n"
              "    SvgKeywords keywords;\n    SvgRange range;\n};\n",
              "\n// Returns what the attribute of an element of `tag` reads as, or false if the element does not read it "
              "as a value.\n",
              "bool svgTypedAttribute(PrDomTag tag, PrSvgAttribute attribute, SvgTypedAttribute* out);\n",
              "\n// Return the value, from 1, of the keyword `text` of the enumeration `keywords`, or 0 if it is none of "
              "its keywords; and the keyword of `value`, or the empty view for a value that no keyword spells.\n",
              "uint8_t svgKeywordValue(SvgKeywords keywords, PrStringView text);\n",
              "PrStringView svgKeywordName(SvgKeywords keywords, uint8_t value);\n",
              "\n// Returns the number of values of the enumeration `keywords`.\n"
              "uint8_t svgKeywordCount(SvgKeywords keywords);\n",
              "\n} // namespace pr_impl\n\nPR_ASSUME_NONNULL_END\n"]
    write(os.path.join(source, "svg_attribute.h"), header)
    count = "countOf(kSvgTypedKey)"
    body = [generated, '\n#include "svg/src/svg_attribute.h"\n', '\n#include "pr/count_of.h"\n#include "pr/name_table.h"\n',
            "\nnamespace pr_impl {\nnamespace {\n", attribute_table.tables(), initials.tables(),
            "\n// The typed attributes, by element and attribute: the tag's value times 65536, plus the attribute's.\n",
            table("constexpr uint32_t kSvgTypedKey[]", keys, 8),
            table("constexpr bool kSvgTypedStated[]", ["true" if row[3] is not None else "false" for row in rows], 12),
            table("constexpr PrSvgValueType kSvgTypedType[]", ["PrSvgValueType_" + row[2] for row in rows], 3),
            table("constexpr SvgAxis kSvgTypedAxis[]", ["SvgAxis_" + row[4] for row in rows], 6),
            table("constexpr SvgKeywords kSvgTypedKeywords[]",
                  [str(keywords.index(row[5]) + 1 if row[5] else 0) for row in rows], 16),
            table("constexpr SvgRange kSvgTypedRange[]", ["SvgRange_" + row[6] for row in rows], 6),
            "\n// The attributes that every element reads, and their types.\n",
            table("constexpr PrSvgAttribute kSvgCommonAttribute[]", [a for a, _ in common], 1),
            table("constexpr PrSvgValueType kSvgCommonType[]", [t for _, t in common], 1),
            word_table.tables(), word_blob.tables(),
            table("constexpr uint8_t kSvgKeywordWordValue[]", [str(v) for v in word_values], 16),
            table("constexpr uint16_t kSvgKeywordStart[]", [str(v) for v in starts], 16),
            "\n} // namespace\n",
            "\nbool svgTypedAttribute(PrDomTag tag, PrSvgAttribute attribute, SvgTypedAttribute* out) {\n"
            "    *out = SvgTypedAttribute{};\n"
            "    uint32_t key = (uint32_t(tag) << 16) | attribute;\n"
            "    uint32_t low = 0;\n    uint32_t high = %s;\n" % count +
            "    while (low < high) {\n        uint32_t middle = (low + high) / 2;\n"
            "        if (kSvgTypedKey[middle] < key)\n            low = middle + 1;\n"
            "        else\n            high = middle;\n    }\n"
            "    if (low < %s && kSvgTypedKey[low] == key) {\n" % count +
            "        out->initial = kSvgTypedStated[low] ? %s : PrStringView{nullptr, 0};\n" % initials.view("low") +
            "        out->type = kSvgTypedType[low];\n        out->axis = kSvgTypedAxis[low];\n"
            "        out->keywords = kSvgTypedKeywords[low];\n        out->range = kSvgTypedRange[low];\n"
            "        return true;\n    }\n"
            "    for (uint32_t i = 0; i < countOf(kSvgCommonAttribute); ++i) {\n"
            "        if (kSvgCommonAttribute[i] == attribute) {\n            out->type = kSvgCommonType[i];\n"
            "            return true;\n        }\n    }\n    return false;\n}\n",
            "\nuint8_t svgKeywordValue(SvgKeywords keywords, PrStringView text) {\n",
            word_table.lookup("text", "index", salt="uint64_t(keywords)"),
            "    if (index == 0 || !%s)\n        return 0;\n" % word_table.confirm("text", word_table.name("index")),
            "    // Two enumerations can share a keyword, so the entry must be of this one.\n"
            "    uint8_t value = kSvgKeywordWordValue[index];\n"
            "    PrStringView found = %s;\n" % word_blob.view("kSvgKeywordStart[keywords] + value - 1"),
            "    return found == text ? value : 0;\n}\n",
            "\nPrStringView svgKeywordName(SvgKeywords keywords, uint8_t value) {\n"
            "    uint32_t at = kSvgKeywordStart[keywords] + value - 1;\n"
            "    if (value == 0 || at >= kSvgKeywordStart[keywords + 1])\n        return PrStringView{};\n"
            "    return %s;\n}\n" % word_blob.view("at"),
            "\nuint8_t svgKeywordCount(SvgKeywords keywords) {\n"
            "    if (keywords == SvgKeywords_None)\n        return 0;\n"
            "    return uint8_t(kSvgKeywordStart[keywords + 1] - kSvgKeywordStart[keywords]);\n}\n",
            "\n} // namespace pr_impl\n\nusing namespace pr_impl;\n",
            "\nPrStringView prSvgAttributeName(PrSvgAttribute attribute) {\n"
            "    if (attribute == PrSvgAttribute_Unknown || attribute >= PrSvgAttribute_Count)\n"
            "        return PrStringView{nullptr, 0};\n    return %s;\n}\n" % attribute_table.name("attribute"),
            "\nPrSvgAttribute prSvgAttributeFromName(PrStringView name) {\n", attribute_table.lookup("name", "index"),
            "    return index != 0 && %s ? PrSvgAttribute(index) : PrSvgAttribute(PrSvgAttribute_Unknown);\n}\n"
            % attribute_table.confirm("name", attribute_table.name("index"))]
    write(os.path.join(source, "svg_attribute.cpp"), body)


def main():
    source = os.path.join(ROOT, "lib/svg/src")
    generated = banner(__file__, "SVG2's attribute index", "https://svgwg.org/svg2-draft/attindex.html")

    # Entry 0 of the attributes is none, which animates discretely and has no initial value.
    attributes = NameTable("kSvgAnimateAttribute", [""] + [name for name, _, _, _ in ATTRIBUTES], lower=False)
    initials = Blob("kSvgAnimateInitial", [""] + [value or "" for _, _, _, value in ATTRIBUTES])
    stated = ["false"] + ["true" if value is not None else "false" for _, _, _, value in ATTRIBUTES]

    header = [generated, '\n#pragma once\n\n#include "pr/base.h"\n#include "pr/svg_types.h"\n\n#include "svg/src/svg_style.h"\n',
              "\nPR_ASSUME_NONNULL_BEGIN\n\nnamespace pr_impl {\n",
              "\n// Return the type that the values of the attribute `name` animate as, a string for one that animates discretely, the axis that a percentage in them "
              "measures against, and the value that the attribute has where an element does not set it, or the null "
              "view if there is none.\n",
              "PrSvgValueType svgAnimateTypeOf(PrStringView name);\nSvgAxis svgAnimateAxisOf(PrStringView name);\n"
              "PrStringView svgAnimateInitialOf(PrStringView name);\n",
              "\n} // namespace pr_impl\n\nPR_ASSUME_NONNULL_END\n"]
    write(os.path.join(source, "svg_animate_names.h"), header)

    body = [generated, '\n#include "svg/src/svg_animate_names.h"\n', '\n#include "pr/name_table.h"\n',
            "\nnamespace pr_impl {\nnamespace {\n",
            attributes.tables(),
            table("constexpr PrSvgValueType kSvgAnimateTypes[]",
                  ["PrSvgValueType_String"] + [TYPES[kind] for _, kind, _, _ in ATTRIBUTES], 3),
            table("constexpr SvgAxis kSvgAnimateAxes[]",
                  ["SvgAxis_Other"] + ["SvgAxis_" + axis for _, _, axis, _ in ATTRIBUTES], 4),
            initials.tables(), table("constexpr bool kSvgAnimateInitialStated[]", stated, 12),
            "\n// Returns the entry of the attribute `name`, or 0 if the index has none.\n",
            "uint32_t attributeOf(PrStringView name) {\n", attributes.lookup("name", "index"),
            "    return %s ? index : 0;\n}\n" % attributes.confirm("name", attributes.name("index")),
            "\n} // namespace\n",
            "\nPrSvgValueType svgAnimateTypeOf(PrStringView name) {\n    return kSvgAnimateTypes[attributeOf(name)];\n}\n",
            "\nSvgAxis svgAnimateAxisOf(PrStringView name) {\n    return kSvgAnimateAxes[attributeOf(name)];\n}\n",
            "\nPrStringView svgAnimateInitialOf(PrStringView name) {\n    uint32_t index = attributeOf(name);\n"
            "    return kSvgAnimateInitialStated[index] ? %s : PrStringView{nullptr, 0};\n}\n" % initials.view("index"),
            "\n} // namespace pr_impl\n"]
    write(os.path.join(source, "svg_animate_names.cpp"), body)
    emit_attributes(source, os.path.join(ROOT, "lib/svg/include/pr"), generated)


if __name__ == "__main__":
    main()
