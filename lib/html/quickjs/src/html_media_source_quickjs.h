// JavaScript binding of Media Source Extensions (media-source-2/).

#pragma once

#include "pr/html_media_source.h"

#include "quickjs.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The classes of MediaSource, SourceBuffer and SourceBufferList.
extern JSClassID htmlMediaSourceClass;
extern JSClassID htmlSourceBufferClass;
extern JSClassID htmlSourceBufferListClass;

// Returns the media source that `value` is the object of, or null with no exception thrown.
PrHtmlMediaSource* PR_NULLABLE jsHtmlMediaSourceOf(JSContext* ctx, JSValueConst value);

// Defines the interfaces of Media Source Extensions in the realm.
void jsHtmlMediaSourceBind(JSContext* ctx);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
