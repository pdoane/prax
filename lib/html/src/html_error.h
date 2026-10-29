// Parse errors
//
// html/#parse-errors - https://html.spec.whatwg.org/multipage/parsing.html#parse-errors

#pragma once

#include "pr/base.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The parse errors that html/#parse-errors names.
enum {
    HtmlErrorCode_AbruptClosingOfEmptyComment,
    HtmlErrorCode_AbruptDoctypePublicIdentifier,
    HtmlErrorCode_AbruptDoctypeSystemIdentifier,
    HtmlErrorCode_AbsenceOfDigitsInNumericCharacterReference,
    HtmlErrorCode_CdataInHtmlContent,
    HtmlErrorCode_CharacterReferenceOutsideUnicodeRange,
    HtmlErrorCode_ControlCharacterInInputStream,
    HtmlErrorCode_ControlCharacterReference,
    HtmlErrorCode_DisallowedProcessingInstructionTarget,
    HtmlErrorCode_DuplicateAttribute,
    HtmlErrorCode_EndTagWithAttributes,
    HtmlErrorCode_EndTagWithTrailingSolidus,
    HtmlErrorCode_EofBeforeTagName,
    HtmlErrorCode_EofInCdata,
    HtmlErrorCode_EofInComment,
    HtmlErrorCode_EofInDoctype,
    HtmlErrorCode_EofInProcessingInstruction,
    HtmlErrorCode_EofInScriptHtmlCommentLikeText,
    HtmlErrorCode_EofInTag,
    HtmlErrorCode_IncorrectlyClosedComment,
    HtmlErrorCode_IncorrectlyOpenedComment,
    HtmlErrorCode_InvalidCharacterSequenceAfterDoctypeName,
    HtmlErrorCode_InvalidFirstCharacterOfProcessingInstructionTarget,
    HtmlErrorCode_InvalidFirstCharacterOfTagName,
    HtmlErrorCode_InvalidProcessingInstructionTarget,
    HtmlErrorCode_MissingAttributeValue,
    HtmlErrorCode_MissingDoctypeName,
    HtmlErrorCode_MissingDoctypePublicIdentifier,
    HtmlErrorCode_MissingDoctypeSystemIdentifier,
    HtmlErrorCode_MissingEndTagName,
    HtmlErrorCode_MissingQuoteBeforeDoctypePublicIdentifier,
    HtmlErrorCode_MissingQuoteBeforeDoctypeSystemIdentifier,
    HtmlErrorCode_MissingSemicolonAfterCharacterReference,
    HtmlErrorCode_MissingWhitespaceAfterDoctypePublicKeyword,
    HtmlErrorCode_MissingWhitespaceAfterDoctypeSystemKeyword,
    HtmlErrorCode_MissingWhitespaceBeforeDoctypeName,
    HtmlErrorCode_MissingWhitespaceBetweenAttributes,
    HtmlErrorCode_MissingWhitespaceBetweenDoctypePublicAndSystemIdentifiers,
    HtmlErrorCode_NestedComment,
    HtmlErrorCode_NonVoidHtmlElementStartTagWithTrailingSolidus,
    HtmlErrorCode_NoncharacterCharacterReference,
    HtmlErrorCode_NoncharacterInInputStream,
    HtmlErrorCode_NullCharacterReference,
    HtmlErrorCode_SurrogateCharacterReference,
    HtmlErrorCode_SurrogateInInputStream,
    HtmlErrorCode_UnexpectedCharacterAfterDoctypeSystemIdentifier,
    HtmlErrorCode_UnexpectedCharacterInAttributeName,
    HtmlErrorCode_UnexpectedCharacterInUnquotedAttributeValue,
    HtmlErrorCode_UnexpectedEqualsSignBeforeAttributeName,
    HtmlErrorCode_UnexpectedNullCharacter,
    HtmlErrorCode_UnexpectedSolidusInTag,
    HtmlErrorCode_UnknownNamedCharacterReference,
    HtmlErrorCode_Unnamed,   // an error that html/#parsing reports without naming it
    HtmlErrorCode_Count,
};
typedef uint8_t HtmlErrorCode;

// A parse error, and where in the input it occurred.
struct HtmlError {
    HtmlErrorCode code;
    uint32_t at;         // the offset of the input byte that provoked the error
};

// The names that html/#parse-errors gives the errors, indexed by code. The name of HtmlErrorCode_Unnamed is the null
// view.
extern const PrStringView kHtmlErrorName[HtmlErrorCode_Count];

} // namespace pr_impl

PR_ASSUME_NONNULL_END
