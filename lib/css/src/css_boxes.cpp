// What elements give their boxes beyond their computed style

#include "css/src/css_boxes.h"

#include "pr/assert.h"

namespace pr_impl {

void cssBoxesProvide(PrDomDocument* document, DomNamespaceId space, const CssBoxProvider* provider) {
    PR_ASSERT(space < kCssProviderNamespaces);
    cssDocumentRoot(document)->boxProviders[space] = provider;
}

CssColorSchemeFlags cssPageSchemes(PrDomDocument* document) {
    const CssRoot* root = document->style;
    for (uint32_t space = 0; root != nullptr && space < kCssProviderNamespaces; ++space) {
        const CssBoxProvider* provider = root->boxProviders[space];
        if (provider != nullptr && provider->pageSchemes != nullptr)
            return provider->pageSchemes(document);
    }
    return 0;
}

} // namespace pr_impl
