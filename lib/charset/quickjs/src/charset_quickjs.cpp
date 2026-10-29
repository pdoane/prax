// JavaScript binding of TextDecoder and TextEncoder.
//
// encoding - https://encoding.spec.whatwg.org/

#include "pr/charset_quickjs.h"

#include "pr/charset.h"
#include "pr/count_of.h"
#include "pr/mem.h"
#include "pr/quickjs_binding.h"
#include "pr/text_buffer.h"
#include "pr/utf8.h"
#include "pr/webidl_quickjs.h"

namespace pr_impl {
namespace {

JSClassID textDecoderClass;
JSClassID textEncoderClass;

// A TextDecoder's decoder, and its do not flush flag (encoding/#textdecoder-do-not-flush-flag). The decoder's charset
// and flags are the decoder's encoding, fatal and ignoreBOM.
struct JsTextDecoder {
    PrCharsetDecoder decoder;
    bool doNotFlush;
};

// A TextEncoder, which holds no state.
struct JsTextEncoder {
    uint8_t _pad;
};

JsTextDecoder* PR_NULLABLE decoderOf(JSContext* ctx, JSValueConst self) {
    return jsValueOf<JsTextDecoder>(ctx, self, textDecoderClass, "TextDecoder");
}

// ---------------------------------------------------------------------------------------------------------------------
// TextDecoder (encoding/#interface-textdecoder)

// encoding/#dom-textdecoder
JSValue constructDecoder(JSContext* ctx, JSValueConst newTarget, int argc, JSValueConst* argv) {
    StringArgument label = {ctx, "utf-8"};
    if (jsArgGiven(argc, argv, 0) && !label.read(argv[0]))
        return JS_EXCEPTION;
    JSValueConst options = argc > 1 ? argv[1] : JS_UNDEFINED;
    bool present = false;
    bool fatal = false;
    bool ignoreBom = false;
    if (!jsDictionary(ctx, options, &present)
        || (present && (!jsDictBool(ctx, options, "fatal", &fatal) || !jsDictBool(ctx, options, "ignoreBOM",
                                                                                     &ignoreBom))))
        return JS_EXCEPTION;
    PrCharset charset = prCharsetFromLabel(label.value());
    if (charset == PrCharset_None || charset == PrCharset_Replacement)
        return JS_ThrowRangeError(ctx, "The encoding label provided ('%.*s') is invalid.", int(label.value().size),
                                  label.value().data);
    PrCharsetDecoderFlags flags = (fatal ? PrCharsetDecoderFlag_Fatal : 0)
                                  | (ignoreBom ? PrCharsetDecoderFlag_KeepBom : 0);
    JsTextDecoder decoder = {.decoder = prCharsetDecoderMake(charset, flags)};
    return jsConstructed(ctx, newTarget, jsNewValue(ctx, textDecoderClass, decoder));
}

// encoding/#dom-textdecoder-decode
JSValue decodeMethod(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    JsTextDecoder* decoder = decoderOf(ctx, self);
    if (decoder == nullptr)
        return JS_EXCEPTION;
    const uint8_t* data = nullptr;
    size_t size = 0;
    if (jsArgGiven(argc, argv, 0) && !jsBufferSource(ctx, argv[0], &data, &size))
        return JS_ThrowTypeError(ctx, "The provided value is not of type '(ArrayBuffer or ArrayBufferView)'");
    JSValueConst options = argc > 1 ? argv[1] : JS_UNDEFINED;
    bool present = false;
    bool stream = false;
    if (!jsDictionary(ctx, options, &present) || (present && !jsDictBool(ctx, options, "stream", &stream)))
        return JS_EXCEPTION;
    if (!decoder->doNotFlush)
        decoder->decoder = prCharsetDecoderMake(decoder->decoder.charset, decoder->decoder.flags);
    decoder->doNotFlush = stream;
    PrStringView bytes = {reinterpret_cast<const char*>(data), size};
    TextBuffer text;
    text.text.resize(uint32_t(prCharsetDecodeMaxSize(&decoder->decoder, size)));
    size_t written = 0;
    bool decoded = prCharsetDecode(&decoder->decoder, bytes, stream ? PrCharsetInput_Partial : PrCharsetInput_Final,
                                   text.text.data, &written);
    JSValue result = decoded ? JS_NewStringLen(ctx, text.text.data, written)
                             : JS_ThrowTypeError(ctx, "The encoded data was not valid.");
    textReset(&text);
    return result;
}

// encoding/#dom-textdecoder-encoding: the name of the decoder's charset in lowercase.
JSValue encodingGet(JSContext* ctx, JSValueConst self) {
    JsTextDecoder* decoder = decoderOf(ctx, self);
    if (decoder == nullptr)
        return JS_EXCEPTION;
    TextBuffer name;
    textAppend(&name, prCharsetName(decoder->decoder.charset));
    textAsciiLower(&name);
    JSValue result = jsNewStringView(ctx, textView(&name));
    textReset(&name);
    return result;
}

// encoding/#dom-textdecoder-fatal and encoding/#dom-textdecoder-ignorebom, by the flag that the magic names.
JSValue flagGet(JSContext* ctx, JSValueConst self, int magic) {
    JsTextDecoder* decoder = decoderOf(ctx, self);
    if (decoder == nullptr)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, (decoder->decoder.flags & magic) != 0);
}

// ---------------------------------------------------------------------------------------------------------------------
// TextEncoder (encoding/#interface-textencoder)

// encoding/#dom-textencoder
JSValue constructEncoder(JSContext* ctx, JSValueConst newTarget, int, JSValueConst*) {
    return jsConstructed(ctx, newTarget, jsNewValue(ctx, textEncoderClass, JsTextEncoder{}));
}

bool isEncoder(JSContext* ctx, JSValueConst self) {
    return jsValueOf<JsTextEncoder>(ctx, self, textEncoderClass, "TextEncoder") != nullptr;
}

// encoding/#dom-textencoder-encoding
JSValue encoderEncodingGet(JSContext* ctx, JSValueConst self) {
    return isEncoder(ctx, self) ? JS_NewString(ctx, "utf-8") : JS_EXCEPTION;
}

// encoding/#dom-textencoder-encode. The string reads as a USVString, which is UTF-8 already.
JSValue encodeMethod(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    StringArgument input = {ctx};
    if (!isEncoder(ctx, self) || (jsArgGiven(argc, argv, 0) && !input.read(argv[0])))
        return JS_EXCEPTION;
    PrStringView text = input.value();
    return JS_NewUint8ArrayCopy(ctx, reinterpret_cast<const uint8_t*>(text.data), text.size);
}

// encoding/#dom-textencoder-encodeinto. Writes the code points of the source that fit in the destination whole, and
// returns how many UTF-16 code units of the source it read and how many bytes it wrote.
JSValue encodeIntoMethod(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv) {
    StringArgument source = {ctx};
    if (!isEncoder(ctx, self) || !jsArgCount(ctx, argc, 2) || !source.read(argv[0]))
        return JS_EXCEPTION;
    if (JS_GetTypedArrayType(argv[1]) != JS_TYPED_ARRAY_UINT8)
        return JS_ThrowTypeError(ctx, "The provided value is not of type 'Uint8Array'");
    size_t offset = 0;
    size_t length = 0;
    size_t elementSize = 0;
    JSValue buffer = JS_GetTypedArrayBuffer(ctx, argv[1], &offset, &length, &elementSize);
    if (JS_IsException(buffer))
        return JS_EXCEPTION;
    size_t total = 0;
    uint8_t* destination = JS_GetArrayBuffer(ctx, &total, buffer);
    JS_FreeValue(ctx, buffer);
    PrStringView text = source.value();
    uint64_t read = 0;
    size_t written = 0;
    for (size_t at = 0; at < text.size && destination != nullptr;) {
        uint32_t codePoint = 0;
        uint32_t width = prUtf8Decode(text, at, &codePoint);
        if (written + width > length)
            break;
        memCopy(destination + offset + written, text.data + at, width);
        written += width;
        at += width;
        read += codePoint > 0xffff ? 2 : 1;
    }
    JSValue result = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, result, "read", JS_NewFloat64(ctx, double(read)));
    JS_SetPropertyStr(ctx, result, "written", JS_NewFloat64(ctx, double(written)));
    return result;
}

constexpr JSCFunctionListEntry kTextDecoderProto[] = {
    PR_JS_TAG("TextDecoder"),
    PR_JS_GET("encoding", encodingGet),
    PR_JS_MAGIC_GET("fatal", flagGet, PrCharsetDecoderFlag_Fatal),
    PR_JS_MAGIC_GET("ignoreBOM", flagGet, PrCharsetDecoderFlag_KeepBom),
    PR_JS_METHOD("decode", 0, decodeMethod),
};

constexpr JSCFunctionListEntry kTextEncoderProto[] = {
    PR_JS_TAG("TextEncoder"),
    PR_JS_GET("encoding", encoderEncodingGet),
    PR_JS_METHOD("encode", 0, encodeMethod),
    PR_JS_METHOD("encodeInto", 2, encodeIntoMethod),
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

void prJsCharsetBind(JSContext* ctx) {
    JSValue decoder = jsDefineClass(ctx, {.name = "TextDecoder",
                                          .id = &textDecoderClass,
                                          .finalizer = jsValueFinalizer,
                                          .mark = jsValueMark,
                                          .proto = kTextDecoderProto,
                                          .protoCount = int(countOf(kTextDecoderProto)),
                                          .constructor = constructDecoder});
    JS_FreeValue(ctx, decoder);
    JSValue encoder = jsDefineClass(ctx, {.name = "TextEncoder",
                                          .id = &textEncoderClass,
                                          .finalizer = jsValueFinalizer,
                                          .mark = jsValueMark,
                                          .proto = kTextEncoderProto,
                                          .protoCount = int(countOf(kTextEncoderProto)),
                                          .constructor = constructEncoder});
    JS_FreeValue(ctx, encoder);
}
