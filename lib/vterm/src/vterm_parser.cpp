// The parser of a terminal's input.

#include "vterm/src/vterm_parser.h"

#include "pr/assert.h"

namespace pr_impl {

namespace {

constexpr uint8_t kEscape = 0x1b;
constexpr uint8_t kBell = 0x07;
constexpr uint8_t kCancel = 0x18;
constexpr uint8_t kSubstitute = 0x1a;
constexpr uint8_t kDelete = 0x7f;

// Begins a new sequence in `state`, with no parameters, intermediates or text.
void begin(VtermParser* parser, VtermParserState state) {
    parser->state = state;
    VtermAction& sequence = parser->sequence;
    sequence.marker = 0;
    sequence.intermediateCount = 0;
    sequence.paramCount = 0;
    sequence.subparams = 0;
    sequence.final = 0;
    parser->paramStarted = false;
    parser->stringOverflowed = false;
    parser->string.clear();
}

// Adds a digit to the current parameter. Starts the first parameter if none has started.
void paramDigit(VtermParser* parser, uint8_t digit) {
    VtermAction& sequence = parser->sequence;
    if (sequence.paramCount == 0) {
        sequence.paramCount = 1;
        sequence.params[0] = kVtermParamNone;
    }
    if (sequence.paramCount > kVtermParserMaxParams)
        return;
    uint32_t& value = sequence.params[sequence.paramCount - 1];
    uint32_t current = value == kVtermParamNone ? 0 : value;
    current = current * 10 + digit;
    value = current > kVtermParserMaxParamValue ? kVtermParserMaxParamValue : current;
    parser->paramStarted = true;
}

// Ends the current parameter at a separator and begins the next. After a colon, the next parameter is a subparameter. A
// separator before any digit ends an empty first parameter.
void paramSeparator(VtermParser* parser, uint8_t separator) {
    VtermAction& sequence = parser->sequence;
    if (sequence.paramCount == 0) {
        sequence.paramCount = 1;
        sequence.params[0] = kVtermParamNone;
    }
    if (sequence.paramCount >= kVtermParserMaxParams) {
        // Parameters past the most are dropped. The count stays one past the last parameter kept, so that the digits
        // that follow are dropped too.
        sequence.paramCount = kVtermParserMaxParams + 1;
        return;
    }
    if (separator == ':')
        sequence.subparams |= 1u << sequence.paramCount;
    sequence.params[sequence.paramCount++] = kVtermParamNone;
    parser->paramStarted = true;
}

// Reads the digits and separators of parameters from `at` in `text`, and returns the offset past them. Parameters are
// most of a sequence, so this is the parser's fast path.
size_t paramsRead(VtermParser* parser, PrStringView text, size_t at) {
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(text.data);
    VtermAction& sequence = parser->sequence;
    while (at < text.size) {
        uint8_t byte = bytes[at];
        if (byte >= '0' && byte <= '9') {
            // The rest of the parameter's digits accumulate in a tight loop, clamped as paramDigit clamps them.
            paramDigit(parser, uint8_t(byte - '0'));
            ++at;
            if (sequence.paramCount > kVtermParserMaxParams)
                continue;
            uint32_t& value = sequence.params[sequence.paramCount - 1];
            uint32_t current = value;
            while (at < text.size && bytes[at] >= '0' && bytes[at] <= '9') {
                current = current * 10 + uint32_t(bytes[at] - '0');
                current = current > kVtermParserMaxParamValue ? kVtermParserMaxParamValue : current;
                ++at;
            }
            value = current;
            continue;
        }
        if (byte == ';' || byte == ':')
            paramSeparator(parser, byte);
        else
            break;
        ++at;
    }
    return at;
}

// Collects an intermediate byte. Returns false if the sequence already holds the most intermediates.
bool collect(VtermParser* parser, uint8_t byte) {
    VtermAction& sequence = parser->sequence;
    if (sequence.intermediateCount >= 2)
        return false;
    sequence.intermediates[sequence.intermediateCount++] = char(byte);
    return true;
}

// Appends a byte to the string in progress. Drops the string once it passes kVtermParserMaxStringBytes.
void stringPut(VtermParser* parser, uint8_t byte) {
    if (parser->stringOverflowed)
        return;
    if (parser->string.count >= kVtermParserMaxStringBytes) {
        parser->stringOverflowed = true;
        parser->string.reset();
        return;
    }
    parser->string.push(char(byte));
}

// Makes the sequence in progress the parser's action of `kind`, with the final byte `final`.
VtermAction* dispatch(VtermParser* parser, VtermActionKind kind, uint8_t final) {
    VtermAction* action = &parser->sequence;
    action->kind = kind;
    action->final = char(final);
    if (action->paramCount > kVtermParserMaxParams)
        action->paramCount = kVtermParserMaxParams;
    parser->state = VtermParserState_Ground;
    return action;
}

// Makes the string in progress the parser's action of `kind`. Returns null if the string overflowed and was dropped.
VtermAction* dispatchString(VtermParser* parser, VtermActionKind kind, uint8_t terminator) {
    if (parser->stringOverflowed)
        return nullptr;
    VtermAction* action = dispatch(parser, kind, uint8_t(parser->sequence.final));
    action->text = PrStringView{parser->string.data, parser->string.count};
    action->terminator = terminator;
    return action;
}

// The action that a string of `state` dispatches.
VtermActionKind stringKind(VtermParserState state) {
    if (state == VtermParserState_OscString)
        return VtermActionKind_Osc;
    if (state == VtermParserState_ApcString)
        return VtermActionKind_Apc;
    return VtermActionKind_Dcs;
}

// Returns the action of a control character. Every state except a string runs a control character as it arrives, and
// the sequence in progress continues. ESC begins an escape sequence instead, and CAN and SUB cancel the sequence.
VtermAction* control(VtermParser* parser, uint8_t byte, VtermAction* scratch) {
    if (byte == kEscape) {
        begin(parser, VtermParserState_Escape);
        return nullptr;
    }
    if (byte == kCancel || byte == kSubstitute) {
        parser->state = VtermParserState_Ground;
        return nullptr;
    }
    scratch->kind = VtermActionKind_Execute;
    scratch->final = char(byte);
    return scratch;
}

// Decodes the non-ASCII text from `*at` into the parser's `printed`, and returns how many code points it wrote. Stops
// at an ASCII byte outside a UTF-8 sequence, or once `printed` is full. A byte that interrupts a sequence ends it with
// a replacement character and is then read again. A sequence that the text ends in continues in the next write.
uint32_t printRun(VtermParser* parser, PrStringView text, size_t* at) {
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(text.data);
    size_t i = *at;
    uint32_t count = 0;
    while (i < text.size && count < kVtermParserMaxPrint) {
        uint8_t byte = bytes[i];
        bool inSequence = prUtf8DecoderIsInSequence(&parser->utf8);
        if (byte < 0x80 && !inSequence)
            break;
        if (!inSequence && text.size - i >= kPrUtf8MaxBytes) {
            // The text holds the whole sequence, so it decodes at once, with the same result as the decoder. Most
            // non-ASCII text is well-formed sequences of two or three bytes, which decode inline here.
            uint8_t second = bytes[i + 1];
            uint8_t third = bytes[i + 2];
            if (byte >= 0xc2 && byte <= 0xdf && (second & 0xc0) == 0x80) {
                parser->printed[count++] = uint32_t(byte & 0x1f) << 6 | (second & 0x3f);
                i += 2;
                continue;
            }
            if (byte >= 0xe0 && byte <= 0xef && (second & 0xc0) == 0x80 && (third & 0xc0) == 0x80) {
                uint32_t codePoint = uint32_t(byte & 0x0f) << 12 | uint32_t(second & 0x3f) << 6 | (third & 0x3f);
                if (codePoint >= 0x800 && (codePoint < 0xd800 || codePoint > 0xdfff)) {
                    parser->printed[count++] = codePoint;
                    i += 3;
                    continue;
                }
            }
            i += prUtf8DecodeScalar(text, i, &parser->printed[count++]);
            continue;
        }
        uint32_t codePoint = kPrUtf8Replacement;
        PrUtf8Step step = prUtf8DecoderPush(&parser->utf8, byte, &codePoint);
        if (step != PrUtf8Step_Interrupted)
            ++i;
        if (step == PrUtf8Step_Continue)
            continue;
        parser->printed[count++] = step == PrUtf8Step_CodePoint ? codePoint : kPrUtf8Replacement;
    }
    *at = i;
    return count;
}

} // namespace

void vtermParserReset(VtermParser* parser) {
    parser->string.reset();
    *parser = VtermParser{};
}

void vtermParserClear(VtermParser* parser) {
    parser->state = VtermParserState_Ground;
    parser->utf8 = PrUtf8Decoder{};
    parser->string.clear();
}

VtermAction* vtermParserNext(VtermParser* parser, PrStringView text, size_t* at) {
    VtermAction& scratch = parser->immediate;
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(text.data);
    size_t size = text.size;
    while (*at < size) {
        uint8_t byte = bytes[*at];

        switch (parser->state) {
            case VtermParserState_Ground: {
                if (byte >= 0x80 || prUtf8DecoderIsInSequence(&parser->utf8)) {
                    uint32_t count = printRun(parser, text, at);
                    if (count == 0)
                        continue;
                    scratch.kind = VtermActionKind_Print;
                    scratch.codePoints = parser->printed;
                    scratch.codePointCount = count;
                    return &scratch;
                }
                if (byte >= 0x20 && byte < kDelete) {
                    size_t start = *at;
                    size_t end = start + 1;
                    while (end < size && bytes[end] >= 0x20 && bytes[end] < kDelete)
                        ++end;
                    *at = end;
                    scratch.kind = VtermActionKind_Ascii;
                    scratch.text = PrStringView{text.data + start, end - start};
                    return &scratch;
                }
                ++*at;
                if (byte < 0x20) {
                    if (VtermAction* action = control(parser, byte, &scratch))
                        return action;
                    continue;
                }
                // The byte is DEL, which a terminal ignores.
                continue;
            }

            case VtermParserState_Escape:
            case VtermParserState_EscapeIntermediate: {
                if (byte >= 0x80) {
                    // A non-ASCII byte ends the sequence, and is read again as text.
                    parser->state = VtermParserState_Ground;
                    continue;
                }
                ++*at;
                if (byte < 0x20) {
                    if (VtermAction* action = control(parser, byte, &scratch))
                        return action;
                    continue;
                }
                if (byte == kDelete)
                    continue;
                if (byte < 0x30) {
                    collect(parser, byte);
                    parser->state = VtermParserState_EscapeIntermediate;
                    continue;
                }
                if (parser->state == VtermParserState_Escape) {
                    switch (byte) {
                        case '[':
                            begin(parser, VtermParserState_CsiEntry);
                            continue;
                        case ']':
                            begin(parser, VtermParserState_OscString);
                            continue;
                        case 'P':
                            begin(parser, VtermParserState_DcsEntry);
                            continue;
                        case '_':
                            begin(parser, VtermParserState_ApcString);
                            continue;
                        case 'X':
                        case '^':
                            begin(parser, VtermParserState_IgnoredString);
                            continue;
                        default:
                            break;
                    }
                }
                return dispatch(parser, VtermActionKind_Escape, byte);
            }

            case VtermParserState_CsiEntry:
            case VtermParserState_CsiParam:
            case VtermParserState_CsiIntermediate:
            case VtermParserState_CsiIgnore:
            case VtermParserState_DcsEntry:
            case VtermParserState_DcsParam:
            case VtermParserState_DcsIntermediate:
            case VtermParserState_DcsIgnore: {
                bool dcs = parser->state >= VtermParserState_DcsEntry;
                VtermParserState state = parser->state;
                if (state == VtermParserState_CsiEntry || state == VtermParserState_CsiParam ||
                        state == VtermParserState_DcsEntry || state == VtermParserState_DcsParam) {
                    size_t end = paramsRead(parser, text, *at);
                    if (end != *at) {
                        *at = end;
                        parser->state = dcs ? VtermParserState_DcsParam : VtermParserState_CsiParam;
                        continue;
                    }
                }
                ++*at;
                if (byte < 0x20) {
                    if (VtermAction* action = control(parser, byte, &scratch))
                        return action;
                    continue;
                }
                if (byte == kDelete || byte >= 0x80)
                    continue;
                VtermParserState ignore = dcs ? VtermParserState_DcsIgnore : VtermParserState_CsiIgnore;
                if (parser->state == ignore) {
                    if (byte >= 0x40) {
                        // An ignored DCS still has a string to skip.
                        parser->state = dcs ? VtermParserState_IgnoredString : VtermParserState_Ground;
                    }
                    continue;
                }
                VtermParserState param = VtermParserState_CsiParam;
                VtermParserState intermediate = VtermParserState_CsiIntermediate;
                if (dcs) {
                    param = VtermParserState_DcsParam;
                    intermediate = VtermParserState_DcsIntermediate;
                }
                bool entry = parser->state == VtermParserState_CsiEntry || parser->state == VtermParserState_DcsEntry;
                if (byte >= '0' && byte <= '9') {
                    if (parser->state == intermediate) {
                        parser->state = ignore;
                        continue;
                    }
                    paramDigit(parser, uint8_t(byte - '0'));
                    parser->state = param;
                    continue;
                }
                if (byte == ';' || byte == ':') {
                    if (parser->state == intermediate) {
                        parser->state = ignore;
                        continue;
                    }
                    paramSeparator(parser, byte);
                    parser->state = param;
                    continue;
                }
                if (byte >= '<' && byte <= '?') {
                    if (!entry) {
                        parser->state = ignore;
                        continue;
                    }
                    parser->sequence.marker = char(byte);
                    parser->state = param;
                    continue;
                }
                if (byte < 0x30) {
                    parser->state = collect(parser, byte) ? intermediate : ignore;
                    continue;
                }
                if (!dcs)
                    return dispatch(parser, VtermActionKind_Csi, byte);
                parser->sequence.final = char(byte);
                parser->state = VtermParserState_DcsString;
                continue;
            }

            case VtermParserState_OscString:
            case VtermParserState_DcsString:
            case VtermParserState_ApcString:
            case VtermParserState_IgnoredString: {
                ++*at;
                bool ignored = parser->state == VtermParserState_IgnoredString;
                if (byte == kEscape || (byte == kBell && parser->state == VtermParserState_OscString)) {
                    // ESC begins the string terminator, ST. The escape state then reads its `\` as a sequence that does
                    // nothing. A string ends at any escape, as in xterm.
                    VtermAction* action = nullptr;
                    if (!ignored)
                        action = dispatchString(parser, stringKind(parser->state), byte == kBell ? kBell : '\\');
                    if (byte == kBell) {
                        parser->state = VtermParserState_Ground;
                    } else {
                        // The escape sequence reuses the string's sequence, so the action is copied out first. Its text
                        // stays valid: a new sequence empties the string's storage but does not free it.
                        if (action != nullptr) {
                            parser->immediate = *action;
                            action = &parser->immediate;
                        }
                        begin(parser, VtermParserState_Escape);
                    }
                    if (action != nullptr)
                        return action;
                    continue;
                }
                if (byte == kCancel || byte == kSubstitute) {
                    parser->state = VtermParserState_Ground;
                    continue;
                }
                if (byte < 0x20 || ignored)
                    continue;
                stringPut(parser, byte);
                continue;
            }
        }
    }
    scratch.kind = VtermActionKind_None;
    return &scratch;
}

uint32_t vtermParam(const VtermAction* action, uint32_t index, uint32_t fallback) {
    if (index >= action->paramCount || action->params[index] == kVtermParamNone)
        return fallback;
    return action->params[index];
}

uint32_t vtermParamCount(const VtermAction* action, uint32_t index, uint32_t fallback) {
    uint32_t value = vtermParam(action, index, fallback);
    return value == 0 ? fallback : value;
}

bool vtermParamIsSub(const VtermAction* action, uint32_t index) {
    return index < action->paramCount && index < 32 && (action->subparams >> index & 1) != 0;
}

} // namespace pr_impl
