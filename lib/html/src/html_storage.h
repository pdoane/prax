// Web Storage: a window's Storage objects and the providers that hold their areas.
//
// html - https://html.spec.whatwg.org/multipage/webstorage.html

#pragma once

#include "pr/html_storage.h"
#include "pr/storage.h"
#include "pr/string.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// A window's Storage object of one kind. Its area is the one that its provider gives `origin`, the origin of the
// window's document when the area was last asked for. A new document of another origin finds its own area.
struct HtmlStorage {
    PrHtmlStorageProvider* PR_NULLABLE provider;
    PrString origin;
    PrStorage* PR_NULLABLE area;
};

// Frees what the Storage object holds. Its area is the provider's.
void htmlStorageReset(HtmlStorage* storage);

} // namespace pr_impl

// C opaque types
struct PrHtmlStorage : pr_impl::HtmlStorage {};

PR_ASSUME_NONNULL_END
