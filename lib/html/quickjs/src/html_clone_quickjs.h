// Structured serialization of JavaScript values (html/#safe-passing-of-structured-data).

#pragma once

#include "pr/html_message.h"
#include "pr/raw_array.h"
#include "pr/raw_string.h"

#include "quickjs.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// A value as bytes that any realm of the runtime can read, and the ports that it transfers, each with a reference. The
// bytes stay readable after the original objects change.
struct JsHtmlSerialized {
    PrRawString bytes;
    RawArray<PrHtmlMessagePort* PR_NONNULL> ports;
};

// Reads a sequence of objects, or the transfer member of a StructuredSerializeOptions, and appends the objects to
// `out`, each with a reference that the caller frees. Returns false with an exception thrown.
bool jsHtmlTransferRead(JSContext* ctx, JSValueConst sequence, RawArray<JSValue>* out);
bool jsHtmlTransferOption(JSContext* ctx, JSValueConst options, RawArray<JSValue>* out);
void jsHtmlTransferReset(JSContext* ctx, RawArray<JSValue>* transfer);

// Serializes `value`, transferring the objects of `transfer`, each detached from its original on success
// (html/#structuredserializewithtransfer). `sender`, if given, is a port that may not transfer itself. Returns false
// with an exception thrown, and then transfers nothing. An unserializable value throws a DataCloneError, and an
// exception that a getter throws propagates.
bool jsHtmlSerialize(JSContext* ctx, JSValueConst value, const RawArray<JSValue>* PR_NULLABLE transfer,
    const PrHtmlMessagePort* PR_NULLABLE sender, JsHtmlSerialized* out);

// Releases what a serialization holds.
void jsHtmlSerializedReset(JsHtmlSerialized* serialized);

// Reads the value that `bytes` hold in the realm of `ctx` (html/#structureddeserialize). `ports` is an array of the
// objects of the ports that the value transferred, in order, or undefined if there are none.
JSValue jsHtmlDeserialize(JSContext* ctx, PrStringView bytes, JSValueConst ports);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
