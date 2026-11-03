// JavaScript binding of the console namespace.
//
// console - https://console.spec.whatwg.org/

#include "pr/html_console_quickjs.h"

#include "pr/array.h"
#include "pr/count_of.h"
#include "pr/float.h"
#include "pr/memory.h"
#include "pr/quickjs_binding.h"
#include "pr/string_ops.h"
#include "pr/text_buffer.h"
#include "pr/time.h"
#include "pr/webidl_quickjs.h"

namespace pr_impl {
namespace {

JSClassID consoleClass;
constexpr int kPrinterKey = 0;

// The count of a label, or the start time of its timer.
struct Labeled {
    PrString label;
    double value;
};

// The state of a realm's console: console/#count-map, console/#timer-table and the depth of console/#group-stack.
struct ConsoleState {
    Array<Labeled> counts;
    Array<Labeled> timers;
    uint32_t depth;
};

void resetLabeled(Array<Labeled>* list) {
    for (uint32_t i = 0; i < list->count; ++i)
        prStringReset(&list->data[i].label);
    list->reset();
}

void consoleFinalizer(JSRuntime*, JSValue stateObject) {
    ConsoleState* state = static_cast<ConsoleState*>(JS_GetOpaque(stateObject, consoleClass));
    if (state == nullptr)
        return;
    resetLabeled(&state->counts);
    resetLabeled(&state->timers);
    prMemoryFree(state);
}

Labeled* PR_NULLABLE findLabel(Array<Labeled>* list, PrStringView label) {
    for (uint32_t i = 0; i < list->count; ++i) {
        if (prStringView(&list->data[i].label) == label)
            return &list->data[i];
    }
    return nullptr;
}

// Appends the value as console/#generic-javascript-object-formatting formats it: the value's string, or a symbol's
// description.
void appendValue(JSContext* ctx, TextBuffer* out, JSValueConst value) {
    JSValue text = JS_IsSymbol(value) ? JS_GetPropertyStr(ctx, value, "description") : JS_ToString(ctx, value);
    JsString read = {};
    if (!JS_IsException(text) && (JS_IsString(text) || JS_IsUndefined(text))) {
        if (JS_IsSymbol(value))
            textAppend(out, "Symbol(");
        if (JS_IsString(text) && jsStringRead(ctx, text, &read))
            textAppend(out, jsStringView(&read));
        if (JS_IsSymbol(value))
            textAppendChar(out, ')');
    } else {
        JS_FreeValue(ctx, JS_GetException(ctx));
        textAppend(out, "[object]");
    }
    jsStringReset(ctx, &read);
    JS_FreeValue(ctx, text);
}

// Calls a global function on one argument, in the way that the formatter's %d and %f call %parseInt% and %parseFloat%.
// Returns NaN for a symbol.
double parsed(JSContext* ctx, const char* function, JSValueConst value) {
    if (JS_IsSymbol(value))
        return kNanF64;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue parse = JS_GetPropertyStr(ctx, global, function);
    JSValue result = JS_Call(ctx, parse, JS_UNDEFINED, 1, &value);
    double number = kNanF64;
    if (JS_IsException(result) || JS_ToFloat64(ctx, &number, result) < 0) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        number = kNanF64;
    }
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, parse);
    JS_FreeValue(ctx, global);
    return number;
}

// Appends a number formatted by Number::toString.
void appendNumber(JSContext* ctx, TextBuffer* out, double number) {
    appendValue(ctx, out, JS_NewFloat64(ctx, number));
}

// Runs console/#formatter over `args`, writing into `out`. Returns the number of arguments consumed.
int format(JSContext* ctx, TextBuffer* out, int argc, JSValueConst* argv) {
    JsString target = {};
    if (!jsStringRead(ctx, argv[0], &target)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return 0;
    }
    PrStringView text = jsStringView(&target);
    int used = 1;
    for (size_t i = 0; i < text.size; ++i) {
        char specifier = i + 1 < text.size && text.data[i] == '%' ? text.data[i + 1] : 0;
        bool known = specifier == 's' || specifier == 'd' || specifier == 'i' || specifier == 'f' ||
            specifier == 'o' || specifier == 'O' || specifier == 'c';
        if (!known || used == argc) {
            textAppendChar(out, text.data[i]);
            continue;
        }
        JSValueConst current = argv[used++];
        switch (specifier) {
            case 'd':
            case 'i': appendNumber(ctx, out, parsed(ctx, "parseInt", current)); break;
            case 'f': appendNumber(ctx, out, parsed(ctx, "parseFloat", current)); break;
            case 'c': break;
            default: appendValue(ctx, out, current); break;
        }
        ++i;
    }
    jsStringReset(ctx, &target);
    return used;
}

void print(JSContext* ctx, PrJsHtmlConsoleLevel level, uint32_t depth, PrStringView text) {
    const PrJsHtmlConsolePrinter* printer =
        static_cast<const PrJsHtmlConsolePrinter*>(jsRuntimeData(JS_GetRuntime(ctx), &kPrinterKey));
    if (printer != nullptr)
        printer->print(ctx, level, depth, text);
}

// console/#logger, after the `prefix` that a method gives its message.
void logger(JSContext* ctx, const ConsoleState* state, PrJsHtmlConsoleLevel level, PrStringView prefix,
        int argc, JSValueConst* argv) {
    if (argc == 0 && prefix.size == 0)
        return;
    TextBuffer out;
    textAppend(&out, prefix);
    int used = 0;
    if (argc > 1 && JS_IsString(argv[0]))
        used = format(ctx, &out, argc, argv);
    for (int i = used; i < argc; ++i) {
        if (i != 0 || prefix.size != 0)
            textAppendChar(&out, ' ');
        appendValue(ctx, &out, argv[i]);
    }
    print(ctx, level, state->depth, textView(&out));
    textReset(&out);
}

ConsoleState* stateOf(JSValueConst* data) {
    return static_cast<ConsoleState*>(JS_GetOpaque(data[0], consoleClass));
}

// Reads the label argument, which is "default" if it is not given.
bool readLabel(int argc, JSValueConst* argv, StringArgument* out) {
    return argc == 0 || JS_IsUndefined(argv[0]) || out->read(argv[0]);
}

enum {
    Method_Log,
    Method_Info,
    Method_Warn,
    Method_Error,
    Method_Debug,
    Method_Trace,
    Method_Dir,
    Method_Dirxml,
    Method_Table,
};
typedef uint8_t Method;

// The logging methods other than assert, each of which prints at its level (console/#logging).
JSValue logMethod(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv, int magic, JSValueConst* data) {
    constexpr PrJsHtmlConsoleLevel kLevels[] = {
        PrJsHtmlConsoleLevel_Log,   PrJsHtmlConsoleLevel_Info,  PrJsHtmlConsoleLevel_Warn,
        PrJsHtmlConsoleLevel_Error, PrJsHtmlConsoleLevel_Debug, PrJsHtmlConsoleLevel_Log,
        PrJsHtmlConsoleLevel_Log,   PrJsHtmlConsoleLevel_Log,   PrJsHtmlConsoleLevel_Log,
    };
    // dir and table print their single argument, not a format.
    int count = magic == Method_Dir || magic == Method_Table ? (argc > 0 ? 1 : 0) : argc;
    logger(ctx, stateOf(data), kLevels[magic], magic == Method_Trace ? "Trace:" : PrStringView{}, count, argv);
    return JS_UNDEFINED;
}

// console/#assert
JSValue assertMethod(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv, int, JSValueConst* data) {
    if (argc > 0 && JS_ToBool(ctx, argv[0]))
        return JS_UNDEFINED;
    logger(ctx, stateOf(data), PrJsHtmlConsoleLevel_Error, "Assertion failed:", argc > 0 ? argc - 1 : 0,
        argc > 0 ? argv + 1 : argv);
    return JS_UNDEFINED;
}

// console/#clear
JSValue clearMethod(JSContext*, JSValueConst, int, JSValueConst*, int, JSValueConst* data) {
    stateOf(data)->depth = 0;
    return JS_UNDEFINED;
}

// console/#count and console/#countreset, selected by the magic.
JSValue countMethod(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv, int magic, JSValueConst* data) {
    ConsoleState* state = stateOf(data);
    StringArgument label = {ctx, "default"};
    if (!readLabel(argc, argv, &label))
        return JS_EXCEPTION;
    Labeled* found = findLabel(&state->counts, label.value());
    TextBuffer text;
    textAppend(&text, label.value());
    if (magic == 0) {
        if (found == nullptr) {
            found = &state->counts.emplace();
            *found = Labeled{prStringCopy(label.value()), 0};
        }
        found->value += 1;
        textAppend(&text, ": ");
        appendNumber(ctx, &text, found->value);
        print(ctx, PrJsHtmlConsoleLevel_Info, state->depth, textView(&text));
    } else if (found != nullptr) {
        found->value = 0;
    } else {
        textAppend(&text, ": no count to reset");
        print(ctx, PrJsHtmlConsoleLevel_Warn, state->depth, textView(&text));
    }
    textReset(&text);
    return JS_UNDEFINED;
}

// console/#group, console/#groupcollapsed and console/#groupend, selected by the magic.
JSValue groupMethod(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv, int magic, JSValueConst* data) {
    ConsoleState* state = stateOf(data);
    if (magic == 2) {
        if (state->depth != 0)
            --state->depth;
        return JS_UNDEFINED;
    }
    if (argc != 0)
        logger(ctx, state, PrJsHtmlConsoleLevel_Log, PrStringView{}, argc, argv);
    ++state->depth;
    return JS_UNDEFINED;
}

// console/#time, console/#timelog and console/#timeend, selected by the magic.
JSValue timeMethod(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv, int magic, JSValueConst* data) {
    ConsoleState* state = stateOf(data);
    StringArgument label = {ctx, "default"};
    if (!readLabel(argc, argv, &label))
        return JS_EXCEPTION;
    Labeled* found = findLabel(&state->timers, label.value());
    TextBuffer text;
    textAppend(&text, label.value());
    if (magic == 0 && found == nullptr) {
        state->timers.push({prStringCopy(label.value()), prTimeMonotonicMilliseconds()});
    } else if (magic == 0 || found == nullptr) {
        textAppend(&text, magic == 0 ? ": timer already exists" : ": no such timer");
        print(ctx, PrJsHtmlConsoleLevel_Warn, state->depth, textView(&text));
    } else {
        textAppend(&text, ": ");
        appendNumber(ctx, &text, prTimeMonotonicMilliseconds() - found->value);
        textAppend(&text, " ms");
        if (magic == 2) {
            prStringReset(&found->label);
            state->timers.erase(uint32_t(found - state->timers.data));
        }
        logger(ctx, state, PrJsHtmlConsoleLevel_Info, textView(&text), magic == 1 && argc > 1 ? argc - 1 : 0, argv + 1);
    }
    textReset(&text);
    return JS_UNDEFINED;
}

// No method has required arguments, so each has length 0.
struct ConsoleMethod {
    const char* name;
    JSCFunctionData* method;
    int magic;
};
constexpr ConsoleMethod kMethods[] = {
    {"assert", JsCallScoped<assertMethod>::call, 0},
    {"clear", JsCallScoped<clearMethod>::call, 0},
    {"debug", JsCallScoped<logMethod>::call, Method_Debug},
    {"error", JsCallScoped<logMethod>::call, Method_Error},
    {"info", JsCallScoped<logMethod>::call, Method_Info},
    {"log", JsCallScoped<logMethod>::call, Method_Log},
    {"table", JsCallScoped<logMethod>::call, Method_Table},
    {"trace", JsCallScoped<logMethod>::call, Method_Trace},
    {"warn", JsCallScoped<logMethod>::call, Method_Warn},
    {"dir", JsCallScoped<logMethod>::call, Method_Dir},
    {"dirxml", JsCallScoped<logMethod>::call, Method_Dirxml},
    {"count", JsCallScoped<countMethod>::call, 0},
    {"countReset", JsCallScoped<countMethod>::call, 1},
    {"group", JsCallScoped<groupMethod>::call, 0},
    {"groupCollapsed", JsCallScoped<groupMethod>::call, 1},
    {"groupEnd", JsCallScoped<groupMethod>::call, 2},
    {"time", JsCallScoped<timeMethod>::call, 0},
    {"timeLog", JsCallScoped<timeMethod>::call, 1},
    {"timeEnd", JsCallScoped<timeMethod>::call, 2},
};

// The list that QuickJS reads the property from when it is first accessed, so the list must outlive the call.
constexpr JSCFunctionListEntry kTag[] = {PR_JS_TAG("console")};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

void prJsHtmlConsoleBind(JSContext* ctx) {
    JSClassDef definition = {"console", consoleFinalizer, nullptr, nullptr, nullptr, nullptr};
    jsRegisterClass(JS_GetRuntime(ctx), &consoleClass, definition);

    // webidl/#namespace-object: its prototype is an object whose prototype is %Object.prototype%.
    JSValue proto = JS_NewObject(ctx);
    JSValue console = JS_NewObjectProtoClass(ctx, proto, consoleClass);
    JS_FreeValue(ctx, proto);
    JS_SetOpaque(console, allocZeroed<ConsoleState>());
    for (const ConsoleMethod& method : kMethods) {
        JSValue function = JS_NewCFunctionData(ctx, method.method, 0, method.magic, 1, &console);
        JS_DefinePropertyValueStr(ctx, console, method.name, function, JS_PROP_C_W_E);
    }
    JS_SetPropertyFunctionList(ctx, console, kTag, int(countOf(kTag)));
    JSValue global = JS_GetGlobalObject(ctx);
    JS_DefinePropertyValueStr(ctx, global, "console", console, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    JS_FreeValue(ctx, global);
}

void prJsHtmlConsoleSetPrinter(JSRuntime* runtime, const PrJsHtmlConsolePrinter* PR_NULLABLE printer) {
    jsSetRuntimeData(runtime, &kPrinterKey, printer);
}
