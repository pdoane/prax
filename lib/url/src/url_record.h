// URL records, parsing and serialization.
//
// url - https://url.spec.whatwg.org/

#pragma once

#include "pr/array.h"
#include "pr/status.h"
#include "pr/string.h"
#include "pr/string_view.h"
#include "pr/url.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// A URL (url/#concept-url). Each component holds its text. A null host, query or fragment is the null string, and an
// empty one is the empty string. A path that is not opaque has `/` before each segment, so the empty path is the empty
// string. Views of a component stay valid until the URL changes.
struct Url {
    PrString scheme;
    PrString username;
    PrString password;
    PrString host;
    PrString path;
    PrString query;
    PrString fragment;
    PrUrlSearchParams* PR_NULLABLE searchParams;    // the query as a list, created on first use
    uint16_t port;
    bool hasPort;
    bool opaquePath;                            // the path is one string rather than segments
};

// A name and value in a query.
struct UrlPair {
    PrString name;
    PrString value;
};

// A list of query pairs (url/#urlsearchparams). `url` is the URL that owns the list and whose query it mirrors, or
// null.
struct UrlSearchParams {
    Array<UrlPair> pairs;
    PrUrl* PR_NULLABLE url;
};

// The states of the basic URL parser. A setter starts the parser in one of them (url/#basic-url-parser-state-override).
enum {
    UrlState_SchemeStart,
    UrlState_Scheme,
    UrlState_NoScheme,
    UrlState_SpecialRelativeOrAuthority,
    UrlState_PathOrAuthority,
    UrlState_Relative,
    UrlState_RelativeSlash,
    UrlState_SpecialAuthoritySlashes,
    UrlState_SpecialAuthorityIgnoreSlashes,
    UrlState_Authority,
    UrlState_Host,
    UrlState_Hostname,
    UrlState_Port,
    UrlState_File,
    UrlState_FileSlash,
    UrlState_FileHost,
    UrlState_PathStart,
    UrlState_Path,
    UrlState_OpaquePath,
    UrlState_Query,
    UrlState_Fragment,
};
typedef uint8_t UrlState;

// Parses UTF-8 `input` into a new URL (url/#concept-basic-url-parser), resolved against `base` if it is not null, with
// its query percent-encoded in `charset`, which is its own output charset. Returns Syntax if `input` does not parse. On
// failure, `*out` is null.
PrStatus urlParse(PrStringView input, const Url* PR_NULLABLE base, PrCharset charset, PrUrl* PR_NULLABLE* PR_NONNULL out);

// Parses `input` into the existing `url`, starting from `state` (url/#basic-url-parser-state-override). A failure
// leaves `url` unchanged.
PrStatus urlParseFrom(Url* url, PrStringView input, UrlState state);

// Creates a copy of the URL without its query list.
PrUrl* urlClone(const Url* url);

// Frees the URL and its query list.
void urlDestroy(Url* PR_NULLABLE url);

// Returns true if the URL's scheme is a special scheme (url/#special-scheme).
bool urlIsSpecial(const Url* url);

// Returns true if the URL cannot have a username, password or port: its host is null or empty, or its scheme is `file`
// (url/#cannot-have-a-username-password-port).
bool urlCannotHaveCredentials(const Url* url);

// Returns the URL as text (url/#concept-url-serializer).
PrString urlSerialize(const Url* url, PrUrlSerialization serialization);

// Returns the URL's origin as text (url/#concept-url-origin, html/#ascii-serialisation-of-an-origin).
PrString urlOrigin(const Url* url);

// Replaces the list with the pairs parsed from `input` (url/#concept-urlencoded-parser). The URL's query is unchanged.
void urlSearchParamsParse(UrlSearchParams* params, PrStringView input);

// Returns the list as `application/x-www-form-urlencoded` text (url/#concept-urlencoded-serializer).
PrString urlSearchParamsSerialize(const UrlSearchParams* params);

// Writes the list into the query of the URL that owns it, if any (url/#concept-urlsearchparams-update). An empty list
// makes the query null.
void urlSearchParamsUpdate(UrlSearchParams* params);

// Removes every pair and keeps the storage.
void urlSearchParamsClear(UrlSearchParams* params);

// Frees the list.
void urlSearchParamsDestroy(UrlSearchParams* PR_NULLABLE params);

} // namespace pr_impl

// C opaque types
struct PrUrl : pr_impl::Url {};
struct PrUrlSearchParams : pr_impl::UrlSearchParams {};

PR_ASSUME_NONNULL_END
