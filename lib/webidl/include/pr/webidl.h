// Web IDL's error names
//
// webidl - https://webidl.spec.whatwg.org/

#pragma once

#include "pr/base.h"
#include "pr/raw_string.h"
#include "pr/status.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The legacy codes of webidl/#dfn-error-names-table, which DOMException's code returns.
enum {
    PrWebIdlErrorCode_IndexSizeErr             = 1,
    PrWebIdlErrorCode_DomstringSizeErr         = 2,
    PrWebIdlErrorCode_HierarchyRequestErr      = 3,
    PrWebIdlErrorCode_WrongDocumentErr         = 4,
    PrWebIdlErrorCode_InvalidCharacterErr      = 5,
    PrWebIdlErrorCode_NoDataAllowedErr         = 6,
    PrWebIdlErrorCode_NoModificationAllowedErr = 7,
    PrWebIdlErrorCode_NotFoundErr              = 8,
    PrWebIdlErrorCode_NotSupportedErr          = 9,
    PrWebIdlErrorCode_InuseAttributeErr        = 10,
    PrWebIdlErrorCode_InvalidStateErr          = 11,
    PrWebIdlErrorCode_SyntaxErr                = 12,
    PrWebIdlErrorCode_InvalidModificationErr   = 13,
    PrWebIdlErrorCode_NamespaceErr             = 14,
    PrWebIdlErrorCode_InvalidAccessErr         = 15,
    PrWebIdlErrorCode_ValidationErr            = 16,
    PrWebIdlErrorCode_TypeMismatchErr          = 17,
    PrWebIdlErrorCode_SecurityErr              = 18,
    PrWebIdlErrorCode_NetworkErr               = 19,
    PrWebIdlErrorCode_AbortErr                 = 20,
    PrWebIdlErrorCode_UrlMismatchErr           = 21,
    PrWebIdlErrorCode_QuotaExceededErr         = 22,
    PrWebIdlErrorCode_TimeoutErr               = 23,
    PrWebIdlErrorCode_InvalidNodeTypeErr       = 24,
    PrWebIdlErrorCode_DataCloneErr             = 25,
};
typedef uint16_t PrWebIdlErrorCode;

// Returns the legacy code of the error name `name`, or 0 for a name that the table does not contain or gives no code.
PrWebIdlErrorCode prWebIdlErrorCode(PrStringView name);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
