// Data transfer: the contract around each backend's reads and writes.

#include "pr/app_data.h"

#include "pr/app_backend.h"
#include "pr/assert.h"
#include "pr/count_of.h"
#include "pr/utf8.h"

#include "app/src/app_names.h"

namespace pr_impl {

namespace {

bool isUtf8(PrStringView text) {
    return prUtf8Validate(text) == text.size;
}

// Checks the content's arrays and text, as each form that it names holds them.
void checkContent(const PrAppDataContent& content) {
    PR_ASSERT((content.forms & PrAppDataFormFlag_Text) == 0 || isUtf8(content.text));
    PR_ASSERT((content.forms & PrAppDataFormFlag_Html) == 0 || isUtf8(content.html));
    PR_ASSERT(content.urls != nullptr || content.urlCount == 0);
    PR_ASSERT(content.paths != nullptr || content.pathCount == 0);
    PR_ASSERT(content.custom != nullptr || content.customCount == 0);
    (void)content;
}

void read(PrApp* app, const PrAppDataRead& request) {
    app->dataReadFn(app, &request);
}

// The names of the enumerations, in the order of their enumerators.
const char* const kDataForms[] = {"Text", "Html", "Urls", "Files", "Image", "Custom", "Payload"};
static_assert(1u << (countOf(kDataForms) - 1) == PrAppDataFormFlag_Payload, "a name for each data form");

const char* const kDragOperations[] = {"None", "Copy", "Move", "Link"};
static_assert(countOf(kDragOperations) == PrAppDragOperation_Link + 1, "a name for each drag operation");
} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrStringView prAppDataFormName(PrAppDataFormFlags flag) {
    return appFlagNameOf(kDataForms, flag);
}

void prAppDataRetain(PrApp* app, PrAppDataSource source) {
    app->dataRetainFn(app, source);
}

void prAppDataRelease(PrApp* app, PrAppDataSource source) {
    app->dataReleaseFn(app, source);
}

void prAppDataReadForms(PrApp* app, PrAppDataSource source, PrAppDataFormsFn done, void* PR_NULLABLE context) {
    read(app, PrAppDataRead{
        .source = source,
        .kind = PrAppDataReadKind_Forms,
        .done = (void (*)(void))done,
        .context = context,
    });
}

void prAppDataReadText(PrApp* app, PrAppDataSource source, PrAppDataTextFn done, void* PR_NULLABLE context) {
    read(app, PrAppDataRead{
        .source = source,
        .kind = PrAppDataReadKind_Text,
        .done = (void (*)(void))done,
        .context = context,
    });
}

void prAppDataReadHtml(PrApp* app, PrAppDataSource source, PrAppDataTextFn done, void* PR_NULLABLE context) {
    read(app, PrAppDataRead{
        .source = source,
        .kind = PrAppDataReadKind_Html,
        .done = (void (*)(void))done,
        .context = context,
    });
}

void prAppDataReadUrls(PrApp* app, PrAppDataSource source, PrAppDataUrlsFn done, void* PR_NULLABLE context) {
    read(app, PrAppDataRead{
        .source = source,
        .kind = PrAppDataReadKind_Urls,
        .done = (void (*)(void))done,
        .context = context,
    });
}

void prAppDataReadFiles(PrApp* app, PrAppDataSource source, PrAppDataFilesFn done, void* PR_NULLABLE context) {
    read(app, PrAppDataRead{
        .source = source,
        .kind = PrAppDataReadKind_Files,
        .done = (void (*)(void))done,
        .context = context,
    });
}

void prAppDataReadImage(PrApp* app, PrAppDataSource source, PrAppDataImageFn done, void* PR_NULLABLE context) {
    read(app, PrAppDataRead{
        .source = source,
        .kind = PrAppDataReadKind_Image,
        .done = (void (*)(void))done,
        .context = context,
    });
}

void prAppDataReadCustom(PrApp* app, PrAppDataSource source, PrStringView type, PrAppDataBytesFn done,
        void* PR_NULLABLE context) {
    PR_ASSERT(type.size != 0);
    read(app, PrAppDataRead{
        .source = source,
        .kind = PrAppDataReadKind_Custom,
        .type = type,
        .done = (void (*)(void))done,
        .context = context,
    });
}

void prAppDataReadFile(PrApp* app, PrAppDataSource source, uint32_t index, uint64_t offset, uint64_t size,
        PrAppDataBytesFn done, void* PR_NULLABLE context) {
    read(app, PrAppDataRead{
        .source = source,
        .kind = PrAppDataReadKind_File,
        .index = index,
        .offset = offset,
        .size = size,
        .done = (void (*)(void))done,
        .context = context,
    });
}

PrStatus prAppDataWrite(PrApp* app, PrAppDataSource source, const PrAppDataContent* content) {
    checkContent(*content);
    PR_ASSERT((content->forms & PrAppDataFormFlag_Image) == 0 ||
        (content->image.extent.x != 0 && content->image.extent.y != 0));
    if (source != kPrAppDataSourceClipboard && source != kPrAppDataSourceSelection)
        return PrStatus_InvalidData;
    return app->dataWriteFn(app, source, content);
}

PrStatus prAppDataWriteText(PrApp* app, PrAppDataSource source, PrStringView text) {
    PrAppDataContent content{.forms = PrAppDataFormFlag_Text, .text = text};
    return prAppDataWrite(app, source, &content);
}

void prAppDataWriteFile(PrApp* app, PrAppDataSource source, uint32_t index, uint64_t offset, PrByteView bytes,
        PrAppDataStatusFn done, void* PR_NULLABLE context) {
    PR_ASSERT(bytes.data != nullptr || bytes.size == 0);
    app->dataWriteFileFn(app, source, index, offset, bytes, done, context);
}

PrStringView prAppDragOperationName(PrAppDragOperation operation) {
    return appNameOf(kDragOperations, operation);
}

void prAppFileDialog(PrApp* app, const PrAppFileDialogDesc* desc, PrAppFileDialogFn done, void* PR_NULLABLE context) {
    PR_ASSERT(desc->kind <= PrAppFileDialogKind_Save);
    PR_ASSERT(desc->types != nullptr || desc->typeCount == 0);
    app->fileDialogFn(app, desc, done, context);
}
