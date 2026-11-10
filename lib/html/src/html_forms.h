// Forms: form controls, forms, and their entry lists
//
// html - https://html.spec.whatwg.org/multipage/forms.html

#pragma once

#include "pr/dom.h"
#include "pr/string_view.h"
#include "pr/text_buffer.h"
#include "pr/text_edit.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Appends a color input's value after the steps of html/#update-a-color-well-control-color, given `text`, the value or
// value attribute that the steps start from.
void htmlColorWellValue(const PrDomElement* input, PrStringView text, PrTextBuffer* out);

// Returns true if the user can edit the text of `element`: a textarea or a text-like input that is neither disabled nor
// read-only.
bool htmlTextControlIsEditable(const PrDomElement* element);

// Replaces the selection, or marks the composition, of an editable text control as the user does, with the events of an
// edit (input-events-2). Each returns true if the text changed. A listener of beforeinput can cancel an edit that is
// not part of a composition. `inputType` names the edit (input-events-2/#interface-InputEvent-Attributes).
bool htmlTextControlInsert(PrDomElement* element, PrStringView text, PrStringView inputType);

// Removes the selection, or the bytes from the caret to `at` if nothing is selected.
bool htmlTextControlDelete(PrDomElement* element, uint32_t at, PrStringView inputType);

// Replaces the composition with `text` as marked text, or with nothing to end it.
void htmlTextControlCompose(PrDomElement* element, PrStringView text);

// Reverts or repeats the user's last edit.
bool htmlTextControlUndo(PrDomElement* element, bool redo);

// Sets the selection as the user's motion does, with its caret's affinity and the position that a vertical motion
// keeps. A selection that the motion changed fires select.
void htmlTextControlSetSelection(PrDomElement* element, const PrTextEditSelection& selection);

// Runs the attribute change steps of a details element (html/#the-details-element) for its attribute `name`, given
// whether the element had the attribute before and has it now. A change of open queues the element's toggle event,
// which coalesces with one that is queued already (html/#queue-a-details-toggle-event-task). Opening closes the other
// details elements of its name group, and a change of name closes the element if another of its new group is open.
void htmlDetailsAttributeChanged(PrDomElement* details, PrStringView name, bool had, bool has);

// Runs the insertion steps of each details element at or under `node` (html/#the-details-element): one that is open
// closes if another of its name group is.
void htmlDetailsInserted(PrDomNode* node);

// Returns true if the element is the summary for its parent details (html/#summary-for-its-parent-details), whose
// activation opens or closes the details.
bool htmlSummaryActivates(const PrDomElement* element);

// Runs the activation behavior of a summary that activates for `click` (html/#the-summary-element): its details opens
// if it is closed, and closes if it is open. A click on interactive content inside the summary
// (html/#interactive-content), such as a link or a form control, leaves the details as it is.
void htmlSummaryActivate(PrDomElement* summary, PrDomEvent* click);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
