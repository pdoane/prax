// Status codes: how a call that can fail ended.

#pragma once

#include "pr/base.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The result of a call that can fail.
enum {
    PrStatus_Ok,
    PrStatus_InvalidData,       // data from outside the program that is malformed or not accepted
    PrStatus_OutOfRange,        // an index past the end
    PrStatus_NotFound,          // nothing by that name
    PrStatus_NotSupported,      // a shape of the API that is not implemented
    PrStatus_NotAllowed,        // well formed, but not permitted here
    PrStatus_InvalidState,      // the object is not in a state that allows it
    PrStatus_ReadOnly,          // the object cannot be modified
    PrStatus_Syntax,            // text that did not parse
    PrStatus_WrongType,         // an argument of a kind the call does not take
    PrStatus_Foreign,           // an argument that belongs to something other than the object
    PrStatus_Limit,             // a resource limit the call runs under was reached
    PrStatus_Truncated,         // the input ended early; what the call wrote from it is usable
    PrStatus_Io,                // reading or writing outside the process failed
    PrStatus_Lost,              // what the object depends on is gone for good, such as a GPU device that was removed
    PrStatus_Count,
};
typedef uint8_t PrStatus;

// Returns the name of `status`. Returns the empty string for PrStatus_Ok, and the null view if `status` is not a
// status.
PrStringView prStatusName(PrStatus status);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
