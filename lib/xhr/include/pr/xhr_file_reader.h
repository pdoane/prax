// FileReader: a script's asynchronous read of a blob (FileAPI/#APIASynch), which reports as XMLHttpRequest does.
//
// FileAPI - https://w3c.github.io/FileAPI/

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"
#include "pr/dom.h"
#include "pr/fetch_blob.h"
#include "pr/string_view.h"
#include "pr/xhr.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A reader of blobs, which delivers each result and its events in tasks of its window. A reader is reference counted.
typedef struct PrXhrFileReader PrXhrFileReader;

// A FileReader as the interface it inherits from.
PR_HANDLE_UPCAST(XhrFileReader, DomEventTarget, EventTarget)

// The states of a reader, with the values of FileReader's constants.
enum {
    PrXhrFileReaderState_Empty,
    PrXhrFileReaderState_Loading,
    PrXhrFileReaderState_Done,
};
typedef uint8_t PrXhrFileReaderState;

// The forms of a reader's result (FileAPI/#readOperation).
enum {
    PrXhrFileReaderFormat_ArrayBuffer,
    PrXhrFileReaderFormat_BinaryString,
    PrXhrFileReaderFormat_Text,
    PrXhrFileReaderFormat_DataUrl,
};
typedef uint8_t PrXhrFileReaderFormat;

// Called with true when a read begins and with false once its last task has run, so that a host can keep what the
// reader's listeners need for that time.
typedef void (*PrXhrFileReaderPendingFn)(void* PR_NULLABLE context, PrXhrFileReader* reader, bool pending);

// Creates a reader whose tasks run on the event loop of `client`, with a reference that the caller holds.
PrXhrFileReader* prXhrFileReaderCreate(const PrXhrClient* client, PrXhrFileReaderPendingFn PR_NULLABLE pending,
    void* PR_NULLABLE context);

// Starts reading `blob` (FileAPI/#readOperation). `encoding` is the label that a Text read prefers, or the null view.
// Returns false if a read is under way, which throws an InvalidStateError.
bool prXhrFileReaderRead(PrXhrFileReader* reader, const PrFetchBlob* blob, PrXhrFileReaderFormat format, PrStringView encoding);

// Ends a read that is under way, and fires its events (FileAPI/#dfn-abort).
void prXhrFileReaderAbort(PrXhrFileReader* reader);

// The reader's state.
PrXhrFileReaderState prXhrFileReaderReadyState(const PrXhrFileReader* reader);

// Returns the number of the reader's current result, which changes each time that the result does.
uint32_t prXhrFileReaderResultVersion(const PrXhrFileReader* reader);

// Returns true if the reader has a result, and writes its form.
bool prXhrFileReaderResultFormat(const PrXhrFileReader* reader, PrXhrFileReaderFormat* out);

// The bytes of an ArrayBuffer result, and the text of any other, valid until the result changes.
PrByteView prXhrFileReaderResultBytes(const PrXhrFileReader* reader);
PrStringView prXhrFileReaderResultText(const PrXhrFileReader* reader);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
