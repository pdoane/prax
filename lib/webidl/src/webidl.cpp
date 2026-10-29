// Web IDL's error names.
//
// webidl - https://webidl.spec.whatwg.org/

#include "pr/webidl.h"

#include "webidl/src/webidl_error.h"

using namespace pr_impl;

PrWebIdlErrorCode prWebIdlErrorCode(PrStringView name) {
    return webidlErrorCode(name);
}
