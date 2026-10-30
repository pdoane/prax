// Web Storage: the items that a page keeps for its origin, in local storage across visits and in session storage for
// the life of its tab.
//
// html - https://html.spec.whatwg.org/multipage/webstorage.html
// storage - https://storage.spec.whatwg.org/

#pragma once

#include "pr/base.h"
#include "pr/html.h"
#include "pr/status.h"
#include "pr/string.h"
#include "pr/string_view.h"
#include "pr/task.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The two kinds of storage (html/#storage-2).
enum {
    PrHtmlStorageKind_Local,     // html/#dom-localstorage
    PrHtmlStorageKind_Session,   // html/#dom-sessionstorage
    PrHtmlStorageKind_Count,
};
typedef uint8_t PrHtmlStorageKind;

// The quota that storage/#registered-storage-endpoints gives an origin's local storage and its session storage, in
// bytes.
enum { kPrHtmlStorageQuotaBytes = 5 << 20 };

// A user agent's storage of one kind (storage/#storage-shed): an area for each origin that asks for one, made when it
// is first asked for. A host gives its browsing contexts a provider of each kind (PrHtmlHost): one for local storage,
// shared by every browsing context, and one for each tab's session storage, which outlives the tab's navigations. A
// provider may be used from any number of threads at once.
typedef struct PrHtmlStorageProvider PrHtmlStorageProvider;

// Where a provider keeps its areas, and how much each may hold.
typedef struct PrHtmlStorageProviderDesc {
    // The UTF-8 path of the directory in which the provider keeps each origin's area across the runs of the program, in
    // a directory of its own. The null view keeps the areas in memory, for as long as the provider lives.
    PrStringView directory;
    uint64_t quotaBytes;    // the bytes that each origin's area may hold, its keys included

    // The pool whose jobs write the areas' files (PrStorageDiskDesc::pool), which a provider on disk must have, and
    // which must outlive the provider. The areas share it with each other and with any other store that the host gives
    // it.
    PrTaskPool* PR_NULLABLE pool;
} PrHtmlStorageProviderDesc;

// Creates a provider. Its directory is made when an origin first asks for an area.
PrHtmlStorageProvider* prHtmlStorageProviderCreate(const PrHtmlStorageProviderDesc* desc);

// Destroys the provider and its areas, after the areas on disk finish their writes. The browsing contexts that use it
// must be destroyed first.
void prHtmlStorageProviderDestroy(PrHtmlStorageProvider* PR_NULLABLE provider);

// Removes every item of every origin's area, including the areas that earlier runs left in the provider's directory, as
// when the user deletes the sites' data. A window whose area is open finds it empty. No storage event fires.
void prHtmlStorageProviderClear(PrHtmlStorageProvider* provider);

// A window's Storage object of one kind (html/#storage-2): the area of its document's origin. The window owns it.
typedef struct PrHtmlStorage PrHtmlStorage;

// Returns the window's Storage object of `kind` (html/#dom-localstorage, html/#dom-sessionstorage). Returns null if the
// document's origin is opaque, the host gives no provider of that kind, or the provider cannot make the origin's area,
// in each case of which the attribute throws a SecurityError.
PrHtmlStorage* PR_NULLABLE prHtmlWindowStorage(PrHtmlWindow* window, PrHtmlStorageKind kind);

// html/#dom-storage-length: the number of items.
uint32_t prHtmlStorageLength(const PrHtmlStorage* self);

// html/#dom-storage-key: the key of the item at `index`, or the null string past the end. The order of the keys is the
// provider's, and stays the same while no item is added or removed.
PrString prHtmlStorageKey(const PrHtmlStorage* self, uint32_t index);

// html/#dom-storage-getitem: the value of the item at `key`, or the null string if there is none.
PrString prHtmlStorageGetItem(const PrHtmlStorage* self, PrStringView key);

// html/#dom-storage-setitem: sets the item at `key` to `value`. Returns Limit, and changes nothing, if the area's quota
// would be exceeded.
PrStatus prHtmlStorageSetItem(PrHtmlStorage* self, PrStringView key, PrStringView value);

// html/#dom-storage-removeitem: removes the item at `key`, if there is one.
void prHtmlStorageRemoveItem(PrHtmlStorage* self, PrStringView key);

// html/#dom-storage-clear: removes every item.
void prHtmlStorageClear(PrHtmlStorage* self);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
