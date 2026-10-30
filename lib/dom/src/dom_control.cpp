// Form controls
//
// html - https://html.spec.whatwg.org/multipage/forms.html

#include "dom/src/dom_control.h"

#include "pr/alloc.h"
#include "pr/ascii.h"
#include "pr/color_space.h"
#include "pr/float.h"
#include "pr/memory.h"
#include "pr/string_ops.h"

#include "dom/src/dom_attribute.h"
#include "dom/src/dom_hooks.h"
#include "dom/src/dom_microsyntax.h"

namespace pr_impl {
namespace {

// The milliseconds of a day, and of a week.
constexpr double kDayMilliseconds = 86400000;
constexpr double kWeekMilliseconds = 604800000;

bool isInput(const PrDomElement* element) {
    return domIsHtml(element, PrDomTag_Input);
}

bool isRadio(const PrDomElement* element) {
    return isInput(element) && domElementInputType(element) == PrDomInputType_Radio;
}

// Returns the nearest ancestor form of `element` in its tree.
PrDomElement* PR_NULLABLE ancestorForm(const PrDomElement* element) {
    for (PrDomNode* at = element->parent; at != nullptr; at = at->parent) {
        PrDomElement* ancestor = domElementFromNode(at);
        if (domIsHtml(ancestor, PrDomTag_Form))
            return ancestor;
    }
    return nullptr;
}

// Returns true if a listed element's form attribute decides its form owner.
bool namesForm(const PrDomElement* element) {
    return domIsListed(element) && domHasAttribute(element, "form");
}

// ---------------------------------------------------------------------------------------------------------------------
// Sanitization (html/#value-sanitization-algorithm)

// Appends `text` without its line feeds and carriage returns (infra/#strip-newlines).
void appendStrippingNewlines(TextBuffer* out, PrStringView text) {
    for (size_t i = 0; i < text.size; ++i) {
        if (text.data[i] != '\n' && text.data[i] != '\r')
            textAppendChar(out, text.data[i]);
    }
}

// Returns the value of the input's number for the input's type, or NaN if `text` does not convert.
double numberOf(const PrDomElement* input, PrStringView text) {
    double value = 0;
    return text.data != nullptr && domInputStringToNumber(input, text, &value) ? value : kNanF64;
}

// The default step, its scale factor and the default step base of an input type to which the step applies
// (html/#concept-input-step-default).
struct StepRules {
    double step;
    double scale;
    double base;
};

StepRules stepRulesOf(PrDomInputType type) {
    switch (type) {
        case PrDomInputType_Date: return StepRules{1, kDayMilliseconds, 0};
        case PrDomInputType_Month: return StepRules{1, 1, 0};
        // The default step base is the start of week 1970-W01.
        case PrDomInputType_Week: return StepRules{1, kWeekMilliseconds, -259200000};
        case PrDomInputType_Time:
        case PrDomInputType_DatetimeLocal: return StepRules{60, 1000, 0};
        default: return StepRules{1, 1, 0};
    }
}

// Returns the value of a range that sanitization gives `value`: the nearest value at a step from the step base that is
// within the minimum and the maximum, or the minimum if the maximum is below it
// (html/#concept-input-value-default-range). Of two nearest values, the one toward positive infinity wins.
double rangeValueOf(const PrDomElement* input, double value) {
    DomInputRange range = domInputRange(input);
    double maximum = range.maximum < range.minimum ? range.minimum : range.maximum;
    if (isNanF64(value))
        value = range.minimum + (maximum - range.minimum) / 2;
    if (value < range.minimum)
        value = range.minimum;
    if (value > maximum)
        value = maximum;
    if (isNanF64(range.step))
        return value;
    double steps = floorF64((value - range.stepBase) / range.step + 0.5);
    double aligned = range.stepBase + steps * range.step;
    // A value past either bound moves one step back inside it, where there is a step inside.
    if (aligned > maximum)
        aligned -= range.step;
    if (aligned < range.minimum)
        aligned += range.step;
    return aligned >= range.minimum && aligned <= maximum ? aligned : value;
}

// Appends the value that the value sanitization algorithm of the input's type makes of `text`, which is the null view
// for a missing value.
void appendSanitized(const PrDomElement* input, PrStringView text, TextBuffer* out) {
    PrStringView value = text.data != nullptr ? text : "";
    switch (domElementInputType(input)) {
        case PrDomInputType_Text:
        case PrDomInputType_Search:
        case PrDomInputType_Tel:
        case PrDomInputType_Password:
            appendStrippingNewlines(out, value);
            return;
        case PrDomInputType_Url: {
            TextBuffer stripped;
            appendStrippingNewlines(&stripped, value);
            textAppend(out, stringTrim(textView(&stripped)));
            textReset(&stripped);
            return;
        }
        case PrDomInputType_Email: {
            TextBuffer stripped;
            appendStrippingNewlines(&stripped, value);
            PrStringView rest = textView(&stripped);
            if (!domHasAttribute(input, "multiple")) {
                textAppend(out, stringTrim(rest));
                textReset(&stripped);
                return;
            }
            // Each comma-separated token is trimmed, and the tokens are joined by single commas.
            for (size_t start = 0;;) {
                size_t comma = start;
                while (comma < rest.size && rest.data[comma] != ',')
                    ++comma;
                textAppend(out, stringTrim(PrStringView{rest.data + start, comma - start}));
                if (comma == rest.size)
                    break;
                textAppendChar(out, ',');
                start = comma + 1;
            }
            textReset(&stripped);
            return;
        }
        case PrDomInputType_Date: {
            PrTimeDate date{};
            if (domParseDate(value, &date))
                textAppend(out, value);
            return;
        }
        case PrDomInputType_Month: {
            int64_t year = 0;
            uint32_t month = 0;
            if (domParseMonth(value, &year, &month))
                textAppend(out, value);
            return;
        }
        case PrDomInputType_Week: {
            int64_t year = 0;
            uint32_t week = 0;
            if (domParseWeek(value, &year, &week))
                textAppend(out, value);
            return;
        }
        case PrDomInputType_Time:
            if (domIsValidTime(value))
                textAppend(out, value);
            return;
        case PrDomInputType_DatetimeLocal: {
            PrTimeDate date{};
            DomTime time = 0;
            uint32_t digits = 0;
            if (domParseLocalDateTime(value, &date, &time, &digits) && digits <= 3)
                domAppendLocalDateTime(out, date, time);
            return;
        }
        case PrDomInputType_Number:
            if (domIsValidFloat(value))
                textAppend(out, value);
            return;
        case PrDomInputType_Range: {
            double number = domIsValidFloat(value) ? numberOf(input, value) : kNanF64;
            domAppendFloat(out, rangeValueOf(input, number));
            return;
        }
        case PrDomInputType_Color:
            domNotifyColorValue(input, text, out);
            return;
        default:
            textAppend(out, value);
            return;
    }
}

// Tells what depends on a control's value that it changed.
void valueChanged(PrDomElement* element) {
    domNotifyStatesChanged(element);
}

// Sets the value of an input in the value mode to the sanitization of `text`, which is the null view for none. Restyles
// the input if the value changed.
void setSanitized(PrDomElement* input, PrStringView text) {
    TextBuffer sanitized;
    appendSanitized(input, text, &sanitized);
    PrStringView value = textView(&sanitized);
    const DomControl* existing = domControl(input);
    PrStringView old = existing != nullptr ? prStringView(&existing->value) : "";
    if (old != value) {
        prStringReplace(&domControlFromElement(input)->value, value);
        valueChanged(input);
    }
    textReset(&sanitized);
}

// Runs the value sanitization algorithm on the current value of an input in the value mode.
void sanitize(PrDomElement* input) {
    const DomControl* control = domControl(input);
    if (control != nullptr && control->dirtyValue) {
        PrString value = prStringClone(&control->value);
        setSanitized(input, prStringView(&value));
        prStringReset(&value);
    } else {
        setSanitized(input, domAttributeValue(input, "value"));
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Radio groups (html/#radio-button-group)

// Returns true if `a` and `b` are radios of one group: both have the same form owner, are in the same tree, and have
// the same name, which is not empty.
bool sameGroup(const PrDomElement* a, PrStringView name, const PrDomElement* b) {
    return a != b && isRadio(b) && domFormOwner(a) == domFormOwner(b) && domAttributeValue(b, "name") == name &&
        domNodeRoot(a) == domNodeRoot(b);
}

template <typename Visit>
void eachInGroup(const PrDomElement* radio, Visit visit) {
    PrStringView name = domAttributeValue(radio, "name");
    if (name.size == 0)
        return;
    if (PrDomElement* form = domFormOwner(radio)) {
        const DomForm* record = domElementState(form)->form;
        // A visit can change the form's list only through script, which unchecking runs none of.
        for (uint32_t i = 0; i < record->controls.count; ++i) {
            PrDomElement* other = record->controls.data[i];
            if (sameGroup(radio, name, other))
                visit(other);
        }
        return;
    }
    PrDomNode* root = domNodeRoot(const_cast<PrDomElement*>(radio));
    for (PrDomNode* node = domNodeNext(root, root); node != nullptr; node = domNodeNext(root, node)) {
        PrDomElement* other = domElementFromNode(node);
        if (other != nullptr && sameGroup(radio, name, other))
            visit(other);
    }
}

// Restyles the radios of the group of `radio`, which match :indeterminate while none of them is checked.
void restyleGroup(const PrDomElement* radio) {
    eachInGroup(radio, [](PrDomElement* other) { domNotifyStatesChanged(other); });
}

// Restyles the radios named `name` in the tree of `root`, after a radio left their group.
void restyleNamed(PrDomNode* root, PrStringView name) {
    for (PrDomNode* node = domNodeNext(root, root); name.size != 0 && node != nullptr; node = domNodeNext(root, node)) {
        PrDomElement* other = domElementFromNode(node);
        if (other != nullptr && isRadio(other) && domAttributeValue(other, "name") == name)
            domNotifyStatesChanged(other);
    }
}

// Unchecks the other radios in the group of `radio` if `radio` is checked.
void uncheckGroup(PrDomElement* radio) {
    if (!isRadio(radio) || !domInputChecked(radio))
        return;
    eachInGroup(radio, [](PrDomElement* other) {
        DomControl* control = other->state != nullptr ? other->state->control : nullptr;
        if (control != nullptr && control->checked) {
            control->checked = false;
            domNotifyStatesChanged(other);
        }
    });
}

// ---------------------------------------------------------------------------------------------------------------------
// Form owners

// Removes the element from the form's list and from its past names map.
void removeFromForm(PrDomElement* form, const PrDomElement* element) {
    DomForm* record = domElementState(form)->form;
    for (uint32_t i = 0; i < record->controls.count; ++i) {
        if (record->controls.data[i] == element) {
            record->controls.data[i] = record->controls.last();
            --record->controls.count;
            break;
        }
    }
    for (uint32_t i = record->pastNames.count; i-- != 0;) {
        if (record->pastNames.data[i].element == element) {
            prStringReset(&record->pastNames.data[i].name);
            record->pastNames.erase(i);
        }
    }
}

// Associates the element with `form`, or with no form for null (html/#concept-form-association).
void setOwner(PrDomElement* element, PrDomElement* PR_NULLABLE form) {
    PrDomElement* old = domFormOwner(element);
    if (old == form)
        return;
    // A radio's group follows its form owner.
    bool radio = isRadio(element);
    if (radio)
        restyleGroup(element);
    if (old != nullptr)
        removeFromForm(old, element);
    domControlFromElement(element)->form = form;
    if (form != nullptr)
        domFormFromElement(form)->controls.push(element);
    domNotifyStatesChanged(element);
    if (radio) {
        uncheckGroup(element);
        restyleGroup(element);
    }
}

// Returns the document's list of listed elements with a form attribute.
Array<PrDomElement* PR_NONNULL>& formAttributedOf(PrDomDocument* document) {
    return document->formAttributed;
}

void forgetFormAttributed(PrDomElement* element) {
    Array<PrDomElement* PR_NONNULL>& list = formAttributedOf(element->document);
    for (uint32_t i = 0; i < list.count; ++i) {
        if (list.data[i] == element) {
            list.erase(i);
            return;
        }
    }
}

// Resets the form owners of the connected controls of the document with a form attribute, after an ID of the document
// changed.
void resetFormAttributed(PrDomDocument* document) {
    Array<PrDomElement* PR_NONNULL>& list = formAttributedOf(document);
    for (uint32_t i = 0; i < list.count; ++i) {
        if (domIsConnected(list.data[i]))
            domFormResetOwner(list.data[i]);
    }
}

// Writes the child text content of `element` to `out`.
void appendChildText(const PrDomElement* element, TextBuffer* out) {
    for (const PrDomNode* child = element->first; child != nullptr; child = child->next) {
        if (child->type == PrDomNodeType_Text || child->type == PrDomNodeType_CdataSection)
            textAppend(out, prStringView(&static_cast<const PrDomCharacterData*>(child)->data));
    }
}

// Appends `text` with each CRLF pair and each CR alone replaced by LF (infra/#normalize-newlines).
void appendNormalizingNewlines(TextBuffer* out, PrStringView text) {
    for (size_t i = 0; i < text.size; ++i) {
        char c = text.data[i];
        if (c == '\r') {
            textAppendChar(out, '\n');
            if (i + 1 < text.size && text.data[i + 1] == '\n')
                ++i;
        } else {
            textAppendChar(out, c);
        }
    }
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// Entry lists

DomEntryList* domEntryListCreate() {
    DomEntryList* list = allocZeroed<DomEntryList>();
    list->references = 1;
    return list;
}

void domEntryListRetain(DomEntryList* list) {
    ++list->references;
}

void domEntryListRelease(DomEntryList* PR_NULLABLE list) {
    if (list == nullptr || --list->references != 0)
        return;
    for (uint32_t i = 0; i < list->entries.count; ++i) {
        prStringReset(&list->entries.data[i].name);
        prStringReset(&list->entries.data[i].value);
    }
    list->entries.reset();
    prMemoryFree(list);
}

void domEntryListAppend(DomEntryList* list, PrStringView name, PrStringView value) {
    list->entries.push(DomEntry{prStringCopy(name), prStringCopy(value)});
}

// ---------------------------------------------------------------------------------------------------------------------
// Categories

bool domIsFormAssociated(const PrDomElement* element) {
    if (element->space != PrDomNamespace_Html)
        return false;
    switch (element->tag) {
        case PrDomTag_Button:
        case PrDomTag_Fieldset:
        case PrDomTag_Img:
        case PrDomTag_Input:
        case PrDomTag_Object:
        case PrDomTag_Output:
        case PrDomTag_Select:
        case PrDomTag_Textarea:
            return true;
        default:
            return false;
    }
}

bool domIsListed(const PrDomElement* element) {
    return domIsFormAssociated(element) && element->tag != PrDomTag_Img;
}

bool domIsSubmittable(const PrDomElement* element) {
    if (element->space != PrDomNamespace_Html)
        return false;
    switch (element->tag) {
        case PrDomTag_Button:
        case PrDomTag_Input:
        case PrDomTag_Select:
        case PrDomTag_Textarea:
            return true;
        default:
            return false;
    }
}

bool domIsResettable(const PrDomElement* element) {
    if (element->space != PrDomNamespace_Html)
        return false;
    switch (element->tag) {
        case PrDomTag_Input:
        case PrDomTag_Output:
        case PrDomTag_Select:
        case PrDomTag_Textarea:
            return true;
        default:
            return false;
    }
}

bool domIsLabelable(const PrDomElement* element) {
    if (element->space != PrDomNamespace_Html)
        return false;
    switch (element->tag) {
        case PrDomTag_Button:
        case PrDomTag_Meter:
        case PrDomTag_Output:
        case PrDomTag_Progress:
        case PrDomTag_Select:
        case PrDomTag_Textarea:
            return true;
        case PrDomTag_Input:
            return domElementInputType(element) != PrDomInputType_Hidden;
        default:
            return false;
    }
}

bool domIsButton(const PrDomElement* element) {
    if (domIsHtml(element, PrDomTag_Button))
        return true;
    if (!isInput(element))
        return false;
    switch (domElementInputType(element)) {
        case PrDomInputType_Submit:
        case PrDomInputType_Image:
        case PrDomInputType_Reset:
        case PrDomInputType_Button:
            return true;
        default:
            return false;
    }
}

bool domIsSubmitButton(const PrDomElement* element) {
    if (isInput(element)) {
        PrDomInputType type = domElementInputType(element);
        return type == PrDomInputType_Submit || type == PrDomInputType_Image;
    }
    if (!domIsHtml(element, PrDomTag_Button))
        return false;
    switch (domButtonType(domAttributeValue(element, "type"))) {
        case DomButtonType_Submit:
            return true;
        case DomButtonType_Auto:
            return !domHasAttribute(element, "command") && !domHasAttribute(element, "commandfor") &&
                !domIsHtml(domElementFromNode(element->parent), PrDomTag_Select);
        default:
            return false;
    }
}

DomControl* domControlFromElement(PrDomElement* element) {
    DomElementState* state = domElementState(element);
    if (state->control == nullptr)
        state->control = allocZeroed<DomControl>();
    return state->control;
}

DomForm* domFormFromElement(PrDomElement* form) {
    DomElementState* state = domElementState(form);
    if (state->form == nullptr)
        state->form = allocZeroed<DomForm>();
    return state->form;
}

const DomControl* PR_NULLABLE domControl(const PrDomElement* element) {
    return element->state != nullptr ? element->state->control : nullptr;
}

void domFormRememberName(PrDomElement* form, PrStringView name, PrDomElement* element) {
    DomForm* record = domFormFromElement(form);
    for (uint32_t i = 0; i < record->pastNames.count; ++i) {
        if (prStringView(&record->pastNames.data[i].name) == name) {
            record->pastNames.data[i].element = element;
            return;
        }
    }
    record->pastNames.push(DomPastName{prStringCopy(name), element});
}

PrDomElement* PR_NULLABLE domFormPastName(const PrDomElement* form, PrStringView name) {
    const DomForm* record = form->state != nullptr ? form->state->form : nullptr;
    for (uint32_t i = 0; record != nullptr && i < record->pastNames.count; ++i) {
        if (prStringView(&record->pastNames.data[i].name) == name)
            return record->pastNames.data[i].element;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------------------------------------------------
// Values

PrDomInputType domElementInputType(const PrDomElement* input) {
    return domInputType(domAttributeValue(input, "type"));
}

DomValueMode domInputValueMode(PrDomInputType type) {
    switch (type) {
        case PrDomInputType_Hidden:
        case PrDomInputType_Submit:
        case PrDomInputType_Image:
        case PrDomInputType_Reset:
        case PrDomInputType_Button:
            return DomValueMode_Default;
        case PrDomInputType_Checkbox:
        case PrDomInputType_Radio:
            return DomValueMode_DefaultOn;
        case PrDomInputType_File:
            return DomValueMode_Filename;
        default:
            return DomValueMode_Value;
    }
}

void domControlValue(const PrDomElement* element, TextBuffer* out) {
    textClear(out);
    if (domIsHtml(element, PrDomTag_Textarea)) {
        const DomControl* control = domControl(element);
        if (control != nullptr && control->dirtyValue) {
            appendNormalizingNewlines(out, prStringView(&control->value));
        } else {
            TextBuffer raw;
            appendChildText(element, &raw);
            appendNormalizingNewlines(out, textView(&raw));
            textReset(&raw);
        }
        return;
    }
    if (!isInput(element))
        return;
    switch (domInputValueMode(domElementInputType(element))) {
        case DomValueMode_Value: {
            const DomControl* control = domControl(element);
            if (control != nullptr)
                textAppend(out, prStringView(&control->value));
            return;
        }
        case DomValueMode_Default:
            textAppend(out, domAttributeValue(element, "value"));
            return;
        case DomValueMode_DefaultOn: {
            PrStringView value = domAttributeValue(element, "value");
            textAppend(out, value.data != nullptr ? value : "on");
            return;
        }
        case DomValueMode_Filename:
            return;
    }
}

bool domControlValueEmpty(const PrDomElement* element) {
    if (domIsHtml(element, PrDomTag_Textarea)) {
        const DomControl* control = domControl(element);
        if (control != nullptr && control->dirtyValue)
            return prStringIsEmpty(&control->value);
        for (const PrDomNode* child = element->first; child != nullptr; child = child->next) {
            if (domIsText(child) && !prStringIsEmpty(&static_cast<const PrDomCharacterData*>(child)->data))
                return false;
        }
        return true;
    }
    if (!isInput(element))
        return true;
    switch (domInputValueMode(domElementInputType(element))) {
        case DomValueMode_Value: {
            const DomControl* control = domControl(element);
            return control == nullptr || prStringIsEmpty(&control->value);
        }
        case DomValueMode_Default:
            return domAttributeValue(element, "value").size == 0;
        case DomValueMode_DefaultOn: {
            PrStringView value = domAttributeValue(element, "value");
            return value.data != nullptr && value.size == 0;
        }
        case DomValueMode_Filename:
            return true;
    }
    return true;
}

PrStatus domControlSetValue(PrDomElement* element, PrStringView value) {
    if (domIsHtml(element, PrDomTag_Textarea)) {
        DomControl* control = domControlFromElement(element);
        prStringReplace(&control->value, value);
        control->dirtyValue = true;
        valueChanged(element);
        return PrStatus_Ok;
    }
    PR_ASSERT(isInput(element));
    switch (domInputValueMode(domElementInputType(element))) {
        case DomValueMode_Value:
            domControlFromElement(element)->dirtyValue = true;
            setSanitized(element, value);
            return PrStatus_Ok;
        case DomValueMode_Default:
        case DomValueMode_DefaultOn:
            domSetAttribute(element, PrDomNamespace_None, "value", value);
            return PrStatus_Ok;
        case DomValueMode_Filename:
            // The input has no files, which a value of the empty string empties.
            return value.size == 0 ? PrStatus_Ok : PrStatus_InvalidState;
    }
    return PrStatus_Ok;
}

void domTextareaDefaultValue(const PrDomElement* textarea, TextBuffer* out) {
    textClear(out);
    appendChildText(textarea, out);
}

void domTextareaChildrenChanged(PrDomElement* element) {
    if (!domIsHtml(element, PrDomTag_Textarea))
        return;
    const DomControl* control = domControl(element);
    if (control == nullptr || !control->dirtyValue)
        valueChanged(element);
}

// ---------------------------------------------------------------------------------------------------------------------
// Numbers, dates and times

DomInputApplyFlags domInputApplies(PrDomInputType type) {
    switch (type) {
        case PrDomInputType_Date:
        case PrDomInputType_Month:
        case PrDomInputType_Week:
        case PrDomInputType_Time:
            return DomInputApplyFlag_ValueAsDate | DomInputApplyFlag_ValueAsNumber;
        case PrDomInputType_DatetimeLocal:
        case PrDomInputType_Number:
        case PrDomInputType_Range:
            return DomInputApplyFlag_ValueAsNumber;
        default:
            return 0;
    }
}

bool domInputStringToNumber(const PrDomElement* input, PrStringView text, double* out) {
    switch (domElementInputType(input)) {
        case PrDomInputType_Date: {
            PrTimeDate date{};
            if (!domParseDate(text, &date))
                return false;
            *out = double(prTimeDateToDays(date)) * kDayMilliseconds;
            return true;
        }
        case PrDomInputType_Month: {
            int64_t year = 0;
            uint32_t month = 0;
            if (!domParseMonth(text, &year, &month))
                return false;
            *out = double((year - 1970) * 12 + int64_t(month) - 1);
            return true;
        }
        case PrDomInputType_Week: {
            int64_t year = 0;
            uint32_t week = 0;
            if (!domParseWeek(text, &year, &week))
                return false;
            *out = double(domWeekDays(year, week)) * kDayMilliseconds;
            return true;
        }
        case PrDomInputType_Time:
            return domParseTime(text, out);
        case PrDomInputType_DatetimeLocal: {
            PrTimeDate date{};
            DomTime time = 0;
            if (!domParseLocalDateTime(text, &date, &time))
                return false;
            *out = double(prTimeDateToDays(date)) * kDayMilliseconds + time;
            return true;
        }
        case PrDomInputType_Number:
        case PrDomInputType_Range:
            return domParseFloat(text, out);
        default:
            return false;
    }
}

void domInputNumberToString(const PrDomElement* input, double value, TextBuffer* out) {
    PR_ASSERT(isFiniteF64(value));
    switch (domElementInputType(input)) {
        case PrDomInputType_Date:
            domAppendDate(out, prTimeDateFromDays(int64_t(floorF64(value / kDayMilliseconds))));
            return;
        case PrDomInputType_Month: {
            int64_t months = int64_t(floorF64(value)) + 1970 * 12;
            int64_t year = months >= 0 ? months / 12 : (months - 11) / 12;
            domAppendMonth(out, year, uint32_t(months - year * 12 + 1));
            return;
        }
        case PrDomInputType_Week: {
            int64_t year = 0;
            uint32_t week = 0;
            domWeekOfDays(int64_t(floorF64(value / kDayMilliseconds)), &year, &week);
            domAppendWeek(out, year, week);
            return;
        }
        case PrDomInputType_Time: {
            double time = value - floorF64(value / kDayMilliseconds) * kDayMilliseconds;
            domAppendTime(out, floorF64(time));
            return;
        }
        case PrDomInputType_DatetimeLocal: {
            double days = floorF64(value / kDayMilliseconds);
            domAppendLocalDateTime(out, prTimeDateFromDays(int64_t(days)), floorF64(value - days * kDayMilliseconds));
            return;
        }
        default:
            domAppendFloat(out, value);
            return;
    }
}

bool domInputStringToDate(const PrDomElement* input, PrStringView text, double* out) {
    switch (domElementInputType(input)) {
        case PrDomInputType_Month: {
            // A month's Date is the start of its first day.
            int64_t year = 0;
            uint32_t month = 0;
            if (!domParseMonth(text, &year, &month))
                return false;
            *out = double(prTimeDateToDays(PrTimeDate{year, month, 1})) * kDayMilliseconds;
            return true;
        }
        case PrDomInputType_Date:
        case PrDomInputType_Week:
        case PrDomInputType_Time:
            return domInputStringToNumber(input, text, out);
        default:
            return false;
    }
}

void domInputDateToString(const PrDomElement* input, double time, TextBuffer* out) {
    if (domElementInputType(input) == PrDomInputType_Month) {
        PrTimeDate date = prTimeDateFromDays(int64_t(floorF64(time / kDayMilliseconds)));
        domAppendMonth(out, date.year, date.month);
        return;
    }
    domInputNumberToString(input, time, out);
}

DomInputRange domInputRange(const PrDomElement* input) {
    PrDomInputType type = domElementInputType(input);
    DomInputRange range{kNanF64, kNanF64, kNanF64, 0};
    double minimum = numberOf(input, domAttributeValue(input, "min"));
    double maximum = numberOf(input, domAttributeValue(input, "max"));
    if (type == PrDomInputType_Range) {
        minimum = isNanF64(minimum) ? 0 : minimum;
        maximum = isNanF64(maximum) ? 100 : maximum;
    }
    range.minimum = minimum;
    range.maximum = maximum;
    StepRules rules = stepRulesOf(type);
    PrStringView step = domAttributeValue(input, "step");
    double parsed = 0;
    if (step.data == nullptr)
        range.step = rules.step * rules.scale;
    else if (asciiEqualsFold(step, "any"))
        range.step = kNanF64;
    else if (!domParseFloat(step, &parsed) || parsed <= 0)
        range.step = rules.step * rules.scale;
    else
        range.step = parsed * rules.scale;
    // A step of months, days or weeks is a whole number of them, and so is a step that rounds below one.
    if (!isNanF64(range.step) &&
            (type == PrDomInputType_Date || type == PrDomInputType_Month || type == PrDomInputType_Week)) {
        double units = floorF64(range.step / rules.scale + 0.5);
        range.step = (units < 1 ? 1 : units) * rules.scale;
    }
    double base = numberOf(input, domAttributeValue(input, "min"));
    if (isNanF64(base))
        base = numberOf(input, domAttributeValue(input, "value"));
    range.stepBase = isNanF64(base) ? rules.base : base;
    return range;
}

PrStatus domInputStep(PrDomElement* input, int32_t n, bool up) {
    if ((domInputApplies(domElementInputType(input)) & DomInputApplyFlag_ValueAsNumber) == 0)
        return PrStatus_InvalidState;
    DomInputRange range = domInputRange(input);
    if (isNanF64(range.step))
        return PrStatus_InvalidState;
    bool bounded = !isNanF64(range.minimum) && !isNanF64(range.maximum);
    if (bounded && range.minimum > range.maximum)
        return PrStatus_Ok;
    // The smallest and the largest value at a step from the step base within the bounds.
    double lowest = range.stepBase + ceilF64((range.minimum - range.stepBase) / range.step) * range.step;
    double highest = range.stepBase + floorF64((range.maximum - range.stepBase) / range.step) * range.step;
    if (bounded && lowest > highest)
        return PrStatus_Ok;
    TextBuffer text;
    domControlValue(input, &text);
    double value = 0;
    if (!domInputStringToNumber(input, textView(&text), &value))
        value = 0;
    double before = value;
    double offset = (value - range.stepBase) / range.step;
    if (offset != floorF64(offset)) {
        value = range.stepBase + (up ? ceilF64(offset) : floorF64(offset)) * range.step;
    } else {
        double delta = range.step * n;
        value += up ? delta : -delta;
    }
    if (!isNanF64(range.minimum) && value < range.minimum)
        value = lowest;
    if (!isNanF64(range.maximum) && value > range.maximum)
        value = highest;
    if ((!up && value > before) || (up && value < before) || !isFiniteF64(value)) {
        textReset(&text);
        return PrStatus_Ok;
    }
    textClear(&text);
    domInputNumberToString(input, value, &text);
    PrStatus status = domControlSetValue(input, textView(&text));
    textReset(&text);
    return status;
}

// ---------------------------------------------------------------------------------------------------------------------
// Checkedness

bool domInputChecked(const PrDomElement* input) {
    const DomControl* control = domControl(input);
    return control != nullptr && control->checked;
}

void domInputSetChecked(PrDomElement* input, bool checked, bool dirty) {
    DomControl* control = domControlFromElement(input);
    if (dirty)
        control->dirtyChecked = true;
    if (control->checked == checked)
        return;
    control->checked = checked;
    domNotifyStatesChanged(input);
    if (isRadio(input)) {
        uncheckGroup(input);
        restyleGroup(input);
    }
}

bool domInputIndeterminate(const PrDomElement* input) {
    const DomControl* control = domControl(input);
    return control != nullptr && control->indeterminate;
}

void domInputSetIndeterminate(PrDomElement* input, bool value) {
    DomControl* control = domControlFromElement(input);
    if (control->indeterminate == value)
        return;
    control->indeterminate = value;
    domNotifyStatesChanged(input);
}

bool domRadioGroupChecked(const PrDomElement* radio) {
    bool checked = false;
    eachInGroup(radio, [&](PrDomElement* other) { checked = checked || domInputChecked(other); });
    return checked;
}

// ---------------------------------------------------------------------------------------------------------------------
// Form owners

PrDomElement* PR_NULLABLE domFormOwner(const PrDomElement* element) {
    const DomControl* control = domControl(element);
    return control != nullptr ? control->form : nullptr;
}

void domFormAssociateParserInserted(PrDomElement* element, PrDomElement* form) {
    PR_ASSERT(domIsFormAssociated(element) && domIsHtml(form, PrDomTag_Form));
    setOwner(element, form);
    domControlFromElement(element)->parserInserted = true;
}

void domFormResetOwner(PrDomElement* element) {
    DomControl* control = element->state != nullptr ? element->state->control : nullptr;
    if (control != nullptr)
        control->parserInserted = false;
    PrDomElement* owner = control != nullptr ? control->form : nullptr;
    bool named = namesForm(element);
    PrDomElement* ancestor = ancestorForm(element);
    if (owner != nullptr && !named && owner == ancestor)
        return;
    PrDomElement* form = nullptr;
    if (named && domIsConnected(element)) {
        PrDomElement* found = domElementById(domNodeRoot(element), domAttributeValue(element, "form"));
        form = domIsHtml(found, PrDomTag_Form) ? found : nullptr;
    } else {
        form = ancestor;
    }
    setOwner(element, form);
}

void domControlsLinked(PrDomNode* node) {
    PrDomDocument* document = node->document;
    if (!document->controls)
        return;
    bool ids = false;
    for (PrDomNode* at = node; at != nullptr; at = domShadowIncludingNext(node, at)) {
        PrDomElement* element = domElementFromNode(at);
        if (element == nullptr)
            continue;
        ids = ids || (element->flags & DomNodeFlag_Id) != 0;
        if (!domIsFormAssociated(element))
            continue;
        const DomControl* control = domControl(element);
        if (control == nullptr || !control->parserInserted)
            domFormResetOwner(element);
        // html/#radio-button-group: a radio that becomes connected keeps its checkedness and takes it from the rest of
        // its group, which no longer matches :indeterminate.
        if (domIsConnected(element) && isRadio(element) && domInputChecked(element)) {
            uncheckGroup(element);
            restyleGroup(element);
        }
    }
    if (ids && domIsConnected(node))
        resetFormAttributed(document);
}

void domControlsUnlinked(PrDomNode* node, PrDomNode* parent) {
    PrDomDocument* document = node->document;
    if (!document->controls)
        return;
    for (PrDomNode* at = node; at != nullptr; at = domShadowIncludingNext(node, at)) {
        PrDomElement* element = domElementFromNode(at);
        if (element == nullptr)
            continue;
        // The radios that the removed radio leaves may now match :indeterminate.
        if (isRadio(element) && domInputChecked(element))
            restyleNamed(domNodeRoot(parent), domAttributeValue(element, "name"));
        PrDomElement* owner = domFormOwner(element);
        if (owner != nullptr && domNodeRoot(element) != domNodeRoot(owner))
            domFormResetOwner(element);
    }
}

void domControlsDisconnected(PrDomNode* node) {
    PrDomDocument* document = node->document;
    if (!document->controls || document->formAttributed.count == 0)
        return;
    for (PrDomNode* at = node; at != nullptr; at = domShadowIncludingNext(node, at)) {
        if ((at->flags & DomNodeFlag_Id) != 0 && at->type == PrDomNodeType_Element) {
            resetFormAttributed(document);
            return;
        }
    }
}

void domControlAttributeChanged(PrDomElement* element, PrStringView name, PrStringView oldValue) {
    if (domIsListed(element) && name == "form") {
        forgetFormAttributed(element);
        if (domHasAttribute(element, "form"))
            formAttributedOf(element->document).push(element);
        domFormResetOwner(element);
        return;
    }
    if (!isInput(element))
        return;
    PrDomInputType type = domElementInputType(element);
    DomValueMode mode = domInputValueMode(type);
    if (name == "value") {
        const DomControl* control = domControl(element);
        if (mode == DomValueMode_Value && (control == nullptr || !control->dirtyValue))
            setSanitized(element, domAttributeValue(element, "value"));
        else if (mode != DomValueMode_Value)
            valueChanged(element);
    } else if (name == "type") {
        // html/#input-type-change
        DomValueMode previous = domInputValueMode(domInputType(oldValue));
        DomControl* control = domControlFromElement(element);
        if (previous == DomValueMode_Value && (mode == DomValueMode_Default || mode == DomValueMode_DefaultOn)) {
            if (!prStringIsEmpty(&control->value)) {
                PrString value = prStringClone(&control->value);
                domSetAttribute(element, PrDomNamespace_None, "value", prStringView(&value));
                prStringReset(&value);
            }
        } else if (previous != DomValueMode_Value && mode == DomValueMode_Value) {
            prStringReset(&control->value);
            control->dirtyValue = false;
        } else if (previous != DomValueMode_Filename && mode == DomValueMode_Filename) {
            prStringReset(&control->value);
        }
        if (mode == DomValueMode_Value)
            sanitize(element);
        // A radio's type change takes its checkedness from the rest of its group.
        uncheckGroup(element);
        domNotifyStatesChanged(element);
    } else if (name == "checked") {
        const DomControl* control = domControl(element);
        if (control == nullptr || !control->dirtyChecked)
            domInputSetChecked(element, domHasAttribute(element, "checked"), false);
    } else if (name == "name") {
        if (type == PrDomInputType_Radio) {
            // The radios that shared the old name lose a member.
            restyleNamed(domNodeRoot(element), oldValue);
            uncheckGroup(element);
            restyleGroup(element);
            domNotifyStatesChanged(element);
        }
    } else if (mode == DomValueMode_Value) {
        // The sanitization of a range reads its bounds and step, that of an email its multiple attribute, and that of a
        // color its alpha and color space.
        bool reads = (type == PrDomInputType_Range && (name == "min" || name == "max" || name == "step")) ||
            (type == PrDomInputType_Email && name == "multiple") ||
            (type == PrDomInputType_Color && (name == "alpha" || name == "colorspace"));
        if (reads)
            sanitize(element);
    }
}

void domControlsIdChanged(PrDomElement* element) {
    if (element->document->controls && domIsConnected(element))
        resetFormAttributed(element->document);
}

void domControlCloned(const PrDomElement* element, PrDomElement* clone) {
    const DomControl* control = domControl(element);
    if (control == nullptr || (!isInput(element) && !domIsHtml(element, PrDomTag_Textarea)))
        return;
    DomControl* copy = domControlFromElement(clone);
    prStringReplace(&copy->value, prStringView(&control->value));
    copy->dirtyValue = control->dirtyValue;
    copy->checked = control->checked;
    copy->dirtyChecked = control->dirtyChecked;
}

void domControlDestroy(PrDomElement* element) {
    DomElementState* state = element->state;
    if (state == nullptr)
        return;
    if (domIsListed(element))
        forgetFormAttributed(element);
    if (DomControl* control = state->control) {
        if (control->form != nullptr)
            removeFromForm(control->form, element);
        prStringReset(&control->value);
        prStringReset(&control->defaultOverride);
        prMemoryFree(control);
        state->control = nullptr;
    }
    if (DomForm* form = state->form) {
        for (uint32_t i = 0; i < form->controls.count; ++i)
            form->controls.data[i]->state->control->form = nullptr;
        form->controls.reset();
        for (uint32_t i = 0; i < form->pastNames.count; ++i)
            prStringReset(&form->pastNames.data[i].name);
        form->pastNames.reset();
        prMemoryFree(form);
        state->form = nullptr;
    }
}

void domControlCreated(PrDomElement* element) {
    if (domIsFormAssociated(element) || domIsHtml(element, PrDomTag_Form))
        element->document->controls = true;
}

void domControlAdopted(PrDomElement* element, PrDomDocument* from) {
    if (!domIsFormAssociated(element) && !domIsHtml(element, PrDomTag_Form))
        return;
    element->document->controls = true;
    Array<PrDomElement* PR_NONNULL>& list = from->formAttributed;
    for (uint32_t i = 0; i < list.count; ++i) {
        if (list.data[i] == element) {
            list.erase(i);
            element->document->formAttributed.push(element);
            return;
        }
    }
}

} // namespace pr_impl
