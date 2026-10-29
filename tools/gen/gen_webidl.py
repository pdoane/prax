#!/usr/bin/env python3
"""Generates the Web IDL error names table (webidl/#dfn-error-names-table), which DOMException's code reads, from
webref's extract of the Web IDL definitions."""

import argparse
import json
import os
import sys

from tools.gen import gen_common
from tools.gen import name_table
from tools.gen.gen_common import namespaced, table, write

# The legacy code of each error name, spelled as the constant in webidl.h. The table gives the other names no code.
ERROR_CODES = {
    "IndexSizeError": "PrWebIdlErrorCode_IndexSizeErr",
    "HierarchyRequestError": "PrWebIdlErrorCode_HierarchyRequestErr",
    "WrongDocumentError": "PrWebIdlErrorCode_WrongDocumentErr",
    "InvalidCharacterError": "PrWebIdlErrorCode_InvalidCharacterErr",
    "NoModificationAllowedError": "PrWebIdlErrorCode_NoModificationAllowedErr",
    "NotFoundError": "PrWebIdlErrorCode_NotFoundErr",
    "NotSupportedError": "PrWebIdlErrorCode_NotSupportedErr",
    "InUseAttributeError": "PrWebIdlErrorCode_InuseAttributeErr",
    "InvalidStateError": "PrWebIdlErrorCode_InvalidStateErr",
    "SyntaxError": "PrWebIdlErrorCode_SyntaxErr",
    "InvalidModificationError": "PrWebIdlErrorCode_InvalidModificationErr",
    "NamespaceError": "PrWebIdlErrorCode_NamespaceErr",
    "InvalidAccessError": "PrWebIdlErrorCode_InvalidAccessErr",
    "TypeMismatchError": "PrWebIdlErrorCode_TypeMismatchErr",
    "SecurityError": "PrWebIdlErrorCode_SecurityErr",
    "NetworkError": "PrWebIdlErrorCode_NetworkErr",
    "AbortError": "PrWebIdlErrorCode_AbortErr",
    "URLMismatchError": "PrWebIdlErrorCode_UrlMismatchErr",
    "TimeoutError": "PrWebIdlErrorCode_TimeoutErr",
    "InvalidNodeTypeError": "PrWebIdlErrorCode_InvalidNodeTypeErr",
    "DataCloneError": "PrWebIdlErrorCode_DataCloneErr",
    "QuotaExceededError": "PrWebIdlErrorCode_QuotaExceededErr",
}


def error_names(webref):
    """Returns the names in webidl/#dfn-error-names-table, which webref extracts as definitions. The table's deprecated
    rows name the interfaces derived from DOMException (webidl/#idl-DOMException-derived-predefineds), which webref
    extracts as those interfaces."""
    with open(os.path.join(webref, "ed", "dfns", "webidl.json"), encoding="utf-8") as f:
        dfns = json.load(f)["dfns"]
    return [d["linkingText"][0] for d in dfns
            if (d["type"] == "exception" and d["heading"]["id"] == "idl-DOMException-error-names")
            or (d["type"] == "interface" and d["heading"]["id"] == "idl-DOMException-derived-predefineds")]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--webref", default=gen_common.WEBREF)
    args = ap.parse_args()

    names = error_names(args.webref)
    for name in ERROR_CODES:
        if name not in names:
            sys.exit("ERROR_CODES names %s, which is no error name" % name)
    typedef = "PrWebIdlErrorCode"
    codes = ["0"] + [ERROR_CODES.get(n, "0") for n in names]
    banner = gen_common.banner(__file__, gen_common.WEBREF_SOURCE, gen_common.webref_url("dfns"))
    source = os.path.join(gen_common.ROOT, "lib", "webidl", "src")

    head = [banner, "\n#pragma once\n", '\n#include "pr/webidl.h"\n']
    body = ["\n// The legacy code that webidl/#dfn-error-names-table gives the error name `name`, or 0 "
            "for a\n// name the table does not hold or gives no code.\n"
            "%s webidlErrorCode(PrStringView name);\n" % typedef]
    write(os.path.join(source, "webidl_error.h"), namespaced(head, body))

    lookup = name_table.NameTable("kWebidlErrorName", [""] + names, lower=False)
    head = [banner, '\n#include "webidl/src/webidl_error.h"\n',
            '\n#include "pr/name_table.h"\n']
    body = ["namespace {\n", lookup.tables(),
            table("constexpr %s kWebidlErrorCode[]" % typedef, codes, 1,
                  ["the empty name"] + names),
            "\n} // namespace\n",
            "\n%s webidlErrorCode(PrStringView name) {\n" % typedef + lookup.lookup("name", "index")
            + "    return %s ? kWebidlErrorCode[index] : 0;\n}\n"
            % lookup.confirm("name", lookup.name("index"))]
    write(os.path.join(source, "webidl_error.cpp"), namespaced(head, body))


if __name__ == "__main__":
    main()
