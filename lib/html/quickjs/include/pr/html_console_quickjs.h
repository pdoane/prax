// JavaScript binding of the console namespace.
//
// console - https://console.spec.whatwg.org/

#pragma once

#include "pr/string_view.h"

#include "quickjs.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The log levels at which a console method prints (console/#loglevel-severity).
enum {
    PrJsHtmlConsoleLevel_Log,
    PrJsHtmlConsoleLevel_Info,
    PrJsHtmlConsoleLevel_Warn,
    PrJsHtmlConsoleLevel_Error,
    PrJsHtmlConsoleLevel_Debug,
};
typedef uint8_t PrJsHtmlConsoleLevel;

// The destination of the console's output (console/#printer). `print` receives one message at a time. `depth` is the
// nesting depth of the message's group, which the printer indents the message by.
typedef struct PrJsHtmlConsolePrinter {
    void (*PR_NONNULL print)(JSContext* ctx, PrJsHtmlConsoleLevel level, uint32_t depth, PrStringView text);
} PrJsHtmlConsolePrinter;

// Defines the console namespace on the global. The namespace prints through the runtime's printer.
void prJsHtmlConsoleBind(JSContext* ctx);

// Sets the runtime's printer. Without one, the console prints nothing.
void prJsHtmlConsoleSetPrinter(JSRuntime* runtime, const PrJsHtmlConsolePrinter* PR_NULLABLE printer);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
