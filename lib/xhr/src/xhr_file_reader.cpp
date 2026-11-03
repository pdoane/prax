// FileReader: a script's asynchronous read of a blob.

#include "xhr/src/xhr_file_reader.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/base64.h"
#include "pr/charset.h"
#include "pr/memory.h"
#include "pr/text_buffer.h"

#include "xhr/src/xhr.h"

namespace pr_impl {

namespace {

void fireProgress(PrXhrFileReader* reader, PrStringView type, uint64_t loaded, uint64_t total) {
    PrXhrProgressEventInit init = prXhrProgressEventInitDefault();
    init.lengthComputable = true;
    init.loaded = double(loaded);
    init.total = double(total);
    PrXhrProgressEvent* event = prXhrProgressEventCreate(type, &init);
    event->trusted = true;
    domEventDispatch(prXhrFileReaderAsEventTarget(reader), event);
    domEventRelease(event);
}

// Decodes a Text read's bytes (FileAPI/#blob-package-data): a byte order mark decides first, then the label that the
// read gave, then the blob's type, then UTF-8.
void decodeText(PrXhrFileReader* reader, PrByteView bytes) {
    PrCharset charset = prCharsetFromLabel(prRawStringView(&reader->encoding));
    if (charset == PrCharset_None)
        charset = prCharsetFromMimeType(prFetchBlobType(reader->blob));
    if (charset == PrCharset_None)
        charset = PrCharset_Utf8;
    PrCharsetDecoder decoder = prCharsetDecoderMake(charset, 0);
    PrRawString decoded = prRawStringNull();
    PrStringView text =
        prCharsetDecodeView(&decoder, PrStringView{reinterpret_cast<const char*>(bytes.data), bytes.size},
            PrCharsetInput_Final, &decoded);
    reader->text = prRawStringCopy(text);
    prRawStringReset(&decoded);
}

// Packages the read's bytes as its format (FileAPI/#blob-package-data).
void packageResult(PrXhrFileReader* reader) {
    PrByteView bytes = prFetchBlobBytes(reader->blob);
    prRawStringReset(&reader->text);
    switch (reader->format) {
        case PrXhrFileReaderFormat_ArrayBuffer:
            break;
        case PrXhrFileReaderFormat_BinaryString: {
            PrTextBuffer text{};
            for (size_t i = 0; i < bytes.size; ++i)
                prTextBufferAppendCodePoint(&text, bytes.data[i]);
            reader->text = prRawStringCopy(prTextBufferView(&text));
            prTextBufferReset(&text);
            break;
        }
        case PrXhrFileReaderFormat_Text:
            decodeText(reader, bytes);
            break;
        case PrXhrFileReaderFormat_DataUrl: {
            PrTextBuffer text{};
            prTextBufferAppend(&text, "data:");
            PrStringView type = prFetchBlobType(reader->blob);
            prTextBufferAppend(&text, type.size != 0 ? type : PrStringView{"application/octet-stream"});
            prTextBufferAppend(&text, ";base64,");
            PrStringView input{reinterpret_cast<const char*>(bytes.data), bytes.size};
            size_t size = prBase64EncodedSize(bytes.size);
            char* encoded = static_cast<char*>(prMemoryAlloc(size != 0 ? size : 1));
            prBase64Encode(input, encoded, size);
            prTextBufferAppend(&text, PrStringView{encoded, size});
            prMemoryFree(encoded);
            reader->text = prRawStringCopy(prTextBufferView(&text));
            prTextBufferReset(&text);
            break;
        }
    }
    reader->hasResult = true;
    ++reader->resultVersion;
}

enum {
    ReadStep_Start,
    ReadStep_Progress,
    ReadStep_Finish,
};
typedef uint8_t ReadStep;

void queueStep(PrXhrFileReader* reader, uint8_t step);

// The reader holds its listeners' object while a task waits, through the client's reference to the task.
void retainTask(void* PR_NULLABLE context) {
    PrXhrFileReader* reader = static_cast<PrXhrFileReader*>(context);
    prDomEventTargetRetain(prXhrFileReaderAsEventTarget(reader));
    if (reader->tasks++ == 0 && reader->pending != nullptr)
        reader->pending(reader->context, reader, true);
}

void releaseTask(void* PR_NULLABLE context) {
    PrXhrFileReader* reader = static_cast<PrXhrFileReader*>(context);
    if (--reader->tasks == 0 && reader->pending != nullptr)
        reader->pending(reader->context, reader, false);
    prDomEventTargetRelease(prXhrFileReaderAsEventTarget(reader));
}

void runStep(void* PR_NULLABLE context) {
    PrXhrFileReader* reader = static_cast<PrXhrFileReader*>(context);
    if (reader->state != PrXhrFileReaderState_Loading || reader->blob == nullptr)
        return;
    uint64_t size = prFetchBlobBytes(reader->blob).size;
    uint32_t generation = reader->generation;
    switch (reader->step) {
        case ReadStep_Start:
            fireProgress(reader, "loadstart", 0, size);
            if (generation == reader->generation)
                queueStep(reader, size != 0 ? ReadStep_Progress : ReadStep_Finish);
            break;
        case ReadStep_Progress:
            fireProgress(reader, "progress", size, size);
            if (generation == reader->generation)
                queueStep(reader, ReadStep_Finish);
            break;
        case ReadStep_Finish:
            reader->state = PrXhrFileReaderState_Done;
            packageResult(reader);
            prFetchBlobRelease(reader->result);
            reader->result = reader->blob;
            reader->blob = nullptr;
            ++reader->generation;
            fireProgress(reader, "load", size, size);
            if (reader->state != PrXhrFileReaderState_Loading)
                fireProgress(reader, "loadend", size, size);
            break;
    }
}

void queueStep(PrXhrFileReader* reader, uint8_t step) {
    reader->step = step;
    PrXhrTask task{.invoke = runStep, .context = reader, .retain = retainTask, .release = releaseTask};
    reader->client.setTimeout(reader->client.context, &task, 0);
}

void destroyReader(DomCountedTarget* target) {
    PrXhrFileReader* reader = static_cast<PrXhrFileReader*>(target);
    prFetchBlobRelease(reader->blob);
    prFetchBlobRelease(reader->result);
    prRawStringReset(&reader->encoding);
    prRawStringReset(&reader->text);
    domListenersDestroy(reader->listeners);
    prMemoryFree(reader);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrXhrFileReader* prXhrFileReaderCreate(const PrXhrClient* client, PrXhrFileReaderPendingFn PR_NULLABLE pending,
        void* PR_NULLABLE context) {
    PrXhrFileReader* reader = allocZeroed<PrXhrFileReader>();
    reader->type = DomTargetType_FileReader;
    reader->references = 1;
    reader->destroy = destroyReader;
    reader->client = *client;
    reader->pending = pending;
    reader->context = context;
    return reader;
}

bool prXhrFileReaderRead(PrXhrFileReader* reader, const PrFetchBlob* blob, PrXhrFileReaderFormat format,
        PrStringView encoding) {
    if (reader->state == PrXhrFileReaderState_Loading)
        return false;
    reader->state = PrXhrFileReaderState_Loading;
    reader->hasResult = false;
    ++reader->resultVersion;
    prFetchBlobRetain(blob);
    prFetchBlobRelease(reader->blob);
    reader->blob = blob;
    reader->format = format;
    prRawStringReset(&reader->encoding);
    if (encoding.data != nullptr)
        reader->encoding = prRawStringCopy(encoding);
    ++reader->generation;
    queueStep(reader, ReadStep_Start);
    return true;
}

void prXhrFileReaderAbort(PrXhrFileReader* reader) {
    if (reader->state != PrXhrFileReaderState_Loading) {
        reader->hasResult = false;
        ++reader->resultVersion;
        return;
    }
    reader->state = PrXhrFileReaderState_Done;
    reader->hasResult = false;
    ++reader->resultVersion;
    ++reader->generation;
    prFetchBlobRelease(reader->blob);
    reader->blob = nullptr;
    fireProgress(reader, "abort", 0, 0);
    if (reader->state != PrXhrFileReaderState_Loading)
        fireProgress(reader, "loadend", 0, 0);
}

PrXhrFileReaderState prXhrFileReaderReadyState(const PrXhrFileReader* reader) {
    return reader->state;
}

uint32_t prXhrFileReaderResultVersion(const PrXhrFileReader* reader) {
    return reader->resultVersion;
}

bool prXhrFileReaderResultFormat(const PrXhrFileReader* reader, PrXhrFileReaderFormat* out) {
    if (reader->hasResult)
        *out = reader->format;
    return reader->hasResult;
}

PrByteView prXhrFileReaderResultBytes(const PrXhrFileReader* reader) {
    PR_ASSERT(reader->result != nullptr);
    return prFetchBlobBytes(reader->result);
}

PrStringView prXhrFileReaderResultText(const PrXhrFileReader* reader) {
    return prRawStringView(&reader->text);
}
