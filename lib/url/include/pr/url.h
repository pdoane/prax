// URL and URLSearchParams
//
// url - https://url.spec.whatwg.org/

#pragma once

#include "pr/base.h"
#include "pr/charset_names.h"
#include "pr/status.h"
#include "pr/string.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A parsed URL. A URL has one owner.
typedef struct PrUrl PrUrl;

// A list of the name-value pairs of a URL query. When a URL owns the list, a change to either one updates the other.
typedef struct PrUrlSearchParams PrUrlSearchParams;

// ---------------------------------------------------------------------------------------------------------------------
// URL

// Parses `input` as a URL (url/#concept-basic-url-parser). If `base` is not null, `input` is resolved against it. The
// query is percent-encoded in the output charset of `charset` (encoding/#get-an-output-encoding): UTF-8 for the URL
// API, and a document's charset for a URL that HTML parses for it (html/#encoding-parsing-a-url). The query of a URL
// that is not special, or whose scheme is ws or wss, is UTF-8 in any charset. Returns Syntax if `input` does not parse.
// On failure, `*out` is null.
PrStatus prUrlCreate(PrStringView input, const PrUrl* PR_NULLABLE base, PrCharset charset,
    PrUrl* PR_NULLABLE* PR_NONNULL out);

// Creates a copy of the URL. The copy has no query object of its own until one is requested.
PrUrl* prUrlClone(const PrUrl* url);

// Frees the URL and its query object.
void prUrlDestroy(PrUrl* PR_NULLABLE url);

// What the URL serializer writes (url/#concept-url-serializer).
enum {
    PrUrlSerialization_Whole,
    PrUrlSerialization_ExcludeFragment,
};
typedef uint8_t PrUrlSerialization;

// Returns the URL as text (url/#concept-url-serializer).
PrString prUrlSerialize(const PrUrl* url, PrUrlSerialization serialization);

// Returns the URL's origin as text (url/#concept-url-origin, html/#ascii-serialisation-of-an-origin).
PrString prUrlOrigin(const PrUrl* url);

// Returns true if the URLs have the same origin (html/#same-origin). An opaque origin, such as a data: URL's, is the
// same as no other.
bool prUrlHasSameOrigin(const PrUrl* url, const PrUrl* other);

// Return the URL's components (url/#concept-url), each as text. The views stay valid until the URL changes. A null
// host, query or fragment is the null view. The path is as url/#url-path-serializer writes it.
PrStringView prUrlScheme(const PrUrl* url);
PrStringView prUrlUsername(const PrUrl* url);
PrStringView prUrlPassword(const PrUrl* url);
PrStringView prUrlHost(const PrUrl* url);
PrStringView prUrlPath(const PrUrl* url);
PrStringView prUrlQuery(const PrUrl* url);
PrStringView prUrlFragment(const PrUrl* url);

// Returns the port, or -1 if the URL has none.
int32_t prUrlPort(const PrUrl* url);

// The attributes of the URL interface (url/#url-class), each a component of the URL that script reads and writes.
enum {
    PrUrlComponent_Href,        // the whole URL
    PrUrlComponent_Origin,      // the origin, which is read only
    PrUrlComponent_Protocol,    // the scheme, followed by `:`
    PrUrlComponent_Username,
    PrUrlComponent_Password,
    PrUrlComponent_Host,        // the host, followed by `:` and the port if there is one
    PrUrlComponent_Hostname,    // the host alone
    PrUrlComponent_Port,
    PrUrlComponent_Pathname,    // the path
    PrUrlComponent_Search,      // `?` and the query, or empty for an empty or null query
    PrUrlComponent_Hash,        // `#` and the fragment, or empty for an empty or null fragment
};
typedef uint8_t PrUrlComponent;

// Returns the value of the attribute for `component`, as the getter of the URL interface returns it. Returns the empty
// string, never the null string, if the URL lacks the component.
PrString prUrlComponent(const PrUrl* url, PrUrlComponent component);

// Sets the attribute for `component` to `value`, following the setter of the URL interface. Href replaces the whole URL
// and returns the same statuses as prUrlCreate. On failure, Href leaves the URL unchanged. Any other component leaves
// the URL unchanged if `value` does not parse. Returns ReadOnly for Origin, which has no setter.
PrStatus prUrlSetComponent(PrUrl* url, PrUrlComponent component, PrStringView value);

// Returns the URL's query as a list of pairs. The URL owns the list and creates it on first use. Changes to either one
// update the other.
PrUrlSearchParams* prUrlSearchParams(PrUrl* url);

// ---------------------------------------------------------------------------------------------------------------------
// URLSearchParams (url/#urlsearchparams)

// Creates an empty list that no URL owns.
PrUrlSearchParams* prUrlSearchParamsCreate(void);

// Frees the list. The list must not be one that a URL owns.
void prUrlSearchParamsDestroy(PrUrlSearchParams* PR_NULLABLE params);

// Replaces the list with the pairs parsed from `input` (url/#concept-urlencoded-parser), and updates the query of the
// URL that owns the list.
void prUrlSearchParamsParse(PrUrlSearchParams* params, PrStringView input);

// Returns the list as `application/x-www-form-urlencoded` text (url/#concept-urlencoded-serializer).
PrString prUrlSearchParamsToString(const PrUrlSearchParams* params);

// Return the number of pairs, and the name and value of the pair at `index`. The views stay valid until the list
// changes.
uint32_t prUrlSearchParamsSize(const PrUrlSearchParams* params);
PrStringView prUrlSearchParamsNameAt(const PrUrlSearchParams* params, uint32_t index);
PrStringView prUrlSearchParamsValueAt(const PrUrlSearchParams* params, uint32_t index);

// Returns the value of the first pair named `name`, or the null view.
PrStringView prUrlSearchParamsGet(const PrUrlSearchParams* params, PrStringView name);

// Returns true if a pair is named `name` and, unless `value` is the null view, has `value`.
bool prUrlSearchParamsHas(const PrUrlSearchParams* params, PrStringView name, PrStringView value);

// These change the list, then update the query of the URL that owns it (url/#concept-urlsearchparams-update). Append
// adds a pair at the end. Delete removes every pair that prUrlSearchParamsHas would match. Set replaces the value of
// the first pair named `name` and removes the other pairs of that name, or appends a pair if there is none. Sort orders
// the pairs by name in UTF-16 code unit order, and keeps the order of pairs with the same name.
void prUrlSearchParamsAppend(PrUrlSearchParams* params, PrStringView name, PrStringView value);
void prUrlSearchParamsDelete(PrUrlSearchParams* params, PrStringView name, PrStringView value);
void prUrlSearchParamsSet(PrUrlSearchParams* params, PrStringView name, PrStringView value);
void prUrlSearchParamsSort(PrUrlSearchParams* params);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
