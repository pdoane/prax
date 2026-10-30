// CSSOM Object Members
//
// cssom-1 - https://drafts.csswg.org/cssom-1/
//
// The parts of itself that an object presents (css_object.h). Each part is a member of the object, and the member's
// address is its handle.

#pragma once

#include "pr/css.h"
#include "pr/dom.h"

#include "css/src/css_rule.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

struct CssRule;
struct CssStyleSheet;

// The rules of a sheet or of a rule's block (cssom-1/#the-cssrulelist-interface). The holder is whichever of the two
// fields is not null.
struct CssRuleList {
    PrCssStyleSheet* PR_NULLABLE sheet;
    PrCssRule* PR_NULLABLE rule;
};

// A CSS declaration block (cssom-1/#the-cssstyledeclaration-interface). The block of a rule presents the rule's
// declarations. The block of an element's style attribute presents `block`.
struct CssStyleDeclaration {
    PrCssRule* PR_NULLABLE parentRule;                  // cssom-1/#cssstyledeclaration-parent-css-rule
    PrDomNode* PR_NULLABLE ownerNode;                   // cssom-1/#cssstyledeclaration-owner-node
    CssDeclarationBlock* PR_NULLABLE block;             // the owner node's declarations

    // Writes the block to the owner node's style attribute.
    void (*PR_NULLABLE updateOwner)(PrCssStyleDeclaration* declaration);

    // Brings the block up to date before each read. Used for a block that presents state kept elsewhere, such as the
    // owner node's computed style (cssom-1/#dom-window-getcomputedstyle). The block is a cache that the declaration
    // owns.
    void (*PR_NULLABLE refresh)(const PrCssStyleDeclaration* declaration);

    // Serializes the value of a longhand or a custom property to `out` without bringing the whole block up to date.
    // Used for a block that `refresh` maintains. The value is the same one that a read of the refreshed block gives.
    // Returns false if it does not serialize the value, and the block is then read instead.
    bool (*PR_NULLABLE serializeValue)(const PrCssStyleDeclaration* declaration, const CssDeclaredName& name,
        TextBuffer* out);

    bool readOnly;                                    // cssom-1/#cssstyledeclaration-readonly-flag
};

// The media list of a sheet, or of an @media or @import rule (cssom-1/#the-medialist-interface). The holder is
// whichever of the two fields is not null.
struct CssMediaList {
    PrCssStyleSheet* PR_NULLABLE sheet;
    PrCssRule* PR_NULLABLE rule;
};

} // namespace pr_impl

// C opaque types. A declaration block's interface is the one that its rule's kind names.
struct PrCssRuleList : pr_impl::CssRuleList {};
struct PrCssStyleDeclaration : pr_impl::CssStyleDeclaration {};
struct PrCssStyleProperties : PrCssStyleDeclaration {};
struct PrCssPageDescriptors : PrCssStyleDeclaration {};
struct PrCssFontFaceDescriptors : PrCssStyleDeclaration {};
struct PrCssPositionTryDescriptors : PrCssStyleDeclaration {};
struct PrCssFunctionDescriptors : PrCssStyleDeclaration {};
struct PrCssMediaList : pr_impl::CssMediaList {};

PR_ASSUME_NONNULL_END
