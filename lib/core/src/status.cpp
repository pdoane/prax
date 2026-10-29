#include "pr/status.h"

namespace pr_impl {

namespace {

constexpr PrStringView kStatusName[] = {
    "",              // PrStatus_Ok
    "InvalidData",   // PrStatus_InvalidData
    "OutOfRange",    // PrStatus_OutOfRange
    "NotFound",      // PrStatus_NotFound
    "NotSupported",  // PrStatus_NotSupported
    "NotAllowed",    // PrStatus_NotAllowed
    "InvalidState",  // PrStatus_InvalidState
    "ReadOnly",      // PrStatus_ReadOnly
    "Syntax",        // PrStatus_Syntax
    "WrongType",     // PrStatus_WrongType
    "Foreign",       // PrStatus_Foreign
    "Limit",         // PrStatus_Limit
    "Truncated",     // PrStatus_Truncated
    "Io",            // PrStatus_Io
    "Lost",          // PrStatus_Lost
};

static_assert(sizeof(kStatusName) == PrStatus_Count * sizeof(PrStringView));

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrStringView prStatusName(PrStatus status) {
    if (status >= PrStatus_Count)
        return PrStringView{};
    return kStatusName[status];
}
