// Media Queries
//
// mediaqueries-5 - https://drafts.csswg.org/mediaqueries-5/
// cssom-view-1 - https://drafts.csswg.org/cssom-view-1/

#pragma once

#include "pr/app_display.h"
#include "pr/app_user_preferences.h"
#include "pr/color.h"
#include "pr/css.h"
#include "pr/dom.h"
#include "pr/status.h"
#include "pr/string.h"
#include "pr/string_view.h"

#include "css/src/css_at_rule.h"
#include "css/src/css_keyword.h"
#include "css/src/css_object.h"
#include "dom/src/dom_event.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// ---------------------------------------------------------------------------------------------------------------------
// Environments

// A media feature's value in an environment, or the value that a query compares it with.
struct CssMediaValue {
    // The number, in the canonical unit of its type (css-values-4/#canonical-unit). A ratio is its quotient. NaN if the
    // environment has no value.
    double number;

    PrCssKeyword keyword;     // PrCssKeyword_Unknown if the value is a number
};

// The number of media features, which are the descriptors from kCssDescriptorMediaFirst up to kCssDescriptorMediaEnd.
constexpr uint32_t kCssMediaFeatureCount = kCssDescriptorMediaEnd - kCssDescriptorMediaFirst;

// The environment that media queries are evaluated against (mediaqueries-5/#media).
struct CssMediaEnvironment {
    PrCssKeyword type;        // the media type (mediaqueries-5/#media-types): screen or print

    // The initial font size in px, against which font-relative lengths resolve (mediaqueries-5/#units).
    double fontSize;

    // The display's HDR headroom in stops, which color-hdr() resolves for (css-color-hdr-1/#hdr-color-function). Script
    // cannot read it.
    float headroom;

    // The display's target pixels per px, which the document renders at. A screen reports it as its resolution, in
    // dppx, but a query's resolution can be set apart from it, and does not change it.
    float pixelRatio;

    // The value of each media feature, indexed from kCssDescriptorMediaFirst. The values of features computed from
    // other features are not read.
    CssMediaValue features[kCssMediaFeatureCount];

    // A script's override of each user preference (mediaqueries-5/#auto-pref), which a query sees instead of the
    // feature's value. The keyword is PrCssKeyword_Unknown for a feature with no override.
    CssMediaValue overrides[kCssMediaFeatureCount];
};

// Initializes a screen with an 800 by 600 viewport at one dot per px, a fine pointer that hovers, sRGB at 8 bits per
// component, and no preferences. Its scripting (mediaqueries-5/#scripting) is enabled if `scripting` is set, and none
// otherwise.
void cssMediaEnvironmentInit(CssMediaEnvironment* environment, bool scripting);

// Sets the media feature `feature` to `value`, written as in a query. `feature` must not be computed from other
// features. Returns false, and leaves the feature unchanged, if the feature's grammar does not accept `value`.
bool cssMediaEnvironmentSet(CssMediaEnvironment* environment, PrCssDescriptor feature, PrStringView value);

// Overrides the user preference `feature` with `keyword`, or removes its override if `keyword` is PrCssKeyword_Unknown
// (mediaqueries-5/#auto-pref).
void cssMediaEnvironmentOverride(CssMediaEnvironment* environment, PrCssDescriptor feature,
                                 PrCssKeyword keyword);

// Returns the keyword of `feature` that a query sees: its override if there is one, and otherwise its value.
PrCssKeyword cssMediaEnvironmentKeyword(const CssMediaEnvironment* environment,
                                        PrCssDescriptor feature);

// Returns the environment's target pixels per px, which the document renders at. Returns 1 if `environment` is null, as
// it is for a document with no window.
float cssMediaEnvironmentPixelRatio(const CssMediaEnvironment* PR_NULLABLE environment);

// Returns the environment's headroom in stops. Returns 0, for SDR, if `environment` is null.
float cssMediaEnvironmentHeadroom(const CssMediaEnvironment* PR_NULLABLE environment);

// Returns true if the user prefers a dark color scheme (mediaqueries-5/#prefers-color-scheme), as a script's override
// says if there is one. Returns false if `environment` is null.
bool cssMediaEnvironmentPrefersDark(const CssMediaEnvironment* PR_NULLABLE environment);

// Sets what describes the display: its target pixel ratio, the features resolution, which reports the ratio in dppx,
// color-gamut and dynamic-range and their video forms, and the headroom in stops that color-hdr() reads. A display
// whose headroom exceeds 1 is high dynamic range. Returns true if any changed.
bool cssMediaEnvironmentSetDisplay(CssMediaEnvironment* environment, const PrAppDisplay& display);

// Sets the user preference features (mediaqueries-5/#mf-user-preferences): prefers-color-scheme, prefers-contrast,
// prefers-reduced-motion and prefers-reduced-transparency. A script's overrides stay above them. Returns true if any
// changed.
bool cssMediaEnvironmentSetUserPreferences(CssMediaEnvironment* environment, const PrAppUserPreferences& preferences);

// Sets the media type.
void cssMediaEnvironmentSetType(CssMediaEnvironment* environment, PrCssMediaType type);

// ---------------------------------------------------------------------------------------------------------------------
// Evaluation

// Returns true if the list is empty or one of its queries is true in the environment. An unknown query counts as false
// (mediaqueries-5/#evaluating).
bool cssMediaListMatches(const CssMediaQueries* list, const CssMediaEnvironment* environment);

// Returns the environment that the sheet's media queries are evaluated against: the environment of its document, or
// null for a sheet in no document.
const CssMediaEnvironment* PR_NULLABLE cssStyleSheetMedia(const PrCssStyleSheet* sheet);

// ---------------------------------------------------------------------------------------------------------------------
// Media query lists

// A MediaQueryList (cssom-view-1/#the-mediaquerylist-interface).
struct CssMediaQueryList : DomCountedTarget {
    const CssMediaEnvironment* environment;   // the window's media environment, which outlives the list
    CssMediaQueries media;
};

// Creates a list of the media queries in `query` (cssom-view-1/#dom-window-matchmedia). Each entry that is not a media
// query becomes `not all`. The caller holds the list's only reference.
PrCssMediaQueryList* cssMediaQueryListCreate(const CssMediaEnvironment* environment,
                                             PrStringView query);

// Adds or releases a reference. The last release frees the list.
void cssMediaQueryListRetain(PrCssMediaQueryList* list);
void cssMediaQueryListRelease(PrCssMediaQueryList* PR_NULLABLE list);

} // namespace pr_impl

// C opaque types
struct PrCssMediaQueryList : pr_impl::CssMediaQueryList {};

PR_ASSUME_NONNULL_END
