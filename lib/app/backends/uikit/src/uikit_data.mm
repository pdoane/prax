// The UIKit backend's data transfer: the clipboard, the files of sources, the document picker and the share sheet.

#include "app/backends/uikit/src/uikit_data.h"

#include "pr/alloc.h"
#include "pr/mem.h"

#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

// tvOS has no clipboard and no document picker.
#if !TARGET_OS_TV

// A document picker that waits for the user's choice, and the request that the choice answers. The host keeps it among
// its presented dialogs until then.
@interface PrUiKitPicker : NSObject <UIDocumentPickerDelegate>
@property(nonatomic) PrApp* app;
@property(nonatomic) uint32_t request;
@property(nonatomic) BOOL directories;
@property(nonatomic, weak) UIViewController* controller;
@end

#endif

namespace pr_impl {

namespace {

#if !TARGET_OS_TV

// Returns the straight-alpha sRGB Rgba8 rows of `image`, with the extent in `extent`, or nil.
NSMutableData* PR_NULLABLE pixelsOf(UIImage* PR_NULLABLE image, PrUVec2* extent) {
    CGImageRef source = image.CGImage;
    if (source == nullptr)
        return nil;
    size_t width = CGImageGetWidth(source);
    size_t height = CGImageGetHeight(source);
    NSMutableData* pixels = [NSMutableData dataWithLength:width * height * 4];
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef context = CGBitmapContextCreate(pixels.mutableBytes, width, height, 8, width * 4, space,
        uint32_t(kCGImageAlphaPremultipliedLast) | uint32_t(kCGBitmapByteOrder32Big));
    CGColorSpaceRelease(space);
    if (context == nullptr)
        return nil;
    CGContextDrawImage(context, CGRectMake(0, 0, CGFloat(width), CGFloat(height)), source);
    CGContextRelease(context);
    // Core Graphics draws premultiplied alpha, and the content is straight.
    uint8_t* bytes = static_cast<uint8_t*>(pixels.mutableBytes);
    for (size_t i = 0; i < width * height; ++i) {
        uint8_t* pixel = bytes + i * 4;
        uint32_t alpha = pixel[3];
        if (alpha == 0 || alpha == 255)
            continue;
        for (uint32_t c = 0; c < 3; ++c)
            pixel[c] = uint8_t(MIN((uint32_t(pixel[c]) * 255 + alpha / 2) / alpha, 255u));
    }
    *extent = PrUVec2{uint32_t(width), uint32_t(height)};
    return pixels;
}

#endif

#if !TARGET_OS_TV

// Returns a PNG of straight-alpha sRGB Rgba8 `image`, or nil.
NSData* PR_NULLABLE pngOf(PrPixelView image) {
    NSMutableData* rows = [NSMutableData dataWithLength:size_t(image.extent.x) * image.extent.y * 4];
    for (uint32_t y = 0; y < image.extent.y; ++y) {
        prMemCopy(static_cast<uint8_t*>(rows.mutableBytes) + size_t(y) * image.extent.x * 4,
            image.data + size_t(y) * image.stride, size_t(image.extent.x) * 4);
    }
    CGDataProviderRef provider = CGDataProviderCreateWithCFData((__bridge CFDataRef)rows);
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGImageRef made = CGImageCreate(image.extent.x, image.extent.y, 8, 32, size_t(image.extent.x) * 4, space,
        uint32_t(kCGImageAlphaLast) | uint32_t(kCGBitmapByteOrder32Big), provider, nullptr, false,
        kCGRenderingIntentDefault);
    CGColorSpaceRelease(space);
    CGDataProviderRelease(provider);
    if (made == nullptr)
        return nil;
    NSData* png = UIImagePNGRepresentation([UIImage imageWithCGImage:made]);
    CGImageRelease(made);
    return png;
}

#endif

// Reads the general pasteboard into `out`: the forms that it holds for a read of the forms, which reads no content and
// so asks the user nothing, or else the content of the form that `kind` reads.
void readPasteboard(AppDataAnswer kind, AppDataStore& out) {
#if TARGET_OS_TV
    (void)kind;
    (void)out;
#else
    UIPasteboard* board = UIPasteboard.generalPasteboard;
    if (kind == PrAppDataReadKind_Forms) {
        if (board.hasStrings)
            out.forms |= PrAppDataFormFlag_Text;
        if (board.hasURLs)
            out.forms |= PrAppDataFormFlag_Urls;
        if (board.hasImages)
            out.forms |= PrAppDataFormFlag_Image;
        if ([board containsPasteboardTypes:@[ UTTypeHTML.identifier ]])
            out.forms |= PrAppDataFormFlag_Html;
        return;
    }
    if (kind == PrAppDataReadKind_Text && board.hasStrings) {
        PrRawString text = uiKitTextOf(board.string);
        appDataStoreSetText(out, prRawStringView(&text));
        prRawStringReset(&text);
    }
    if (kind == PrAppDataReadKind_Urls && board.hasURLs) {
        for (NSURL* url in board.URLs) {
            PrRawString text = uiKitTextOf(url.absoluteString);
            appDataStoreAddUrl(out, prRawStringView(&text));
            prRawStringReset(&text);
        }
    }
    if (kind == PrAppDataReadKind_Html) {
        id html = [board valueForPasteboardType:UTTypeHTML.identifier];
        if ([html isKindOfClass:NSData.class])
            html = [[NSString alloc] initWithData:html encoding:NSUTF8StringEncoding];
        if ([html isKindOfClass:NSString.class]) {
            PrRawString text = uiKitTextOf(html);
            appDataStoreSetHtml(out, prRawStringView(&text));
            prRawStringReset(&text);
        }
    }
    if (kind == PrAppDataReadKind_Image && board.hasImages) {
        PrUVec2 extent{};
        if (NSMutableData* pixels = pixelsOf(board.image, &extent)) {
            appDataStoreSetImage(out, PrPixelView{
                static_cast<const uint8_t*>(pixels.bytes), size_t(extent.x) * 4,
                extent,
            });
        }
    }
#endif
}

// Answers the request `id` of content from the main queue: from the general pasteboard, or from a source's store.
void readContent(PrApp* app, uint32_t id, PrAppDataSource source, AppDataAnswer kind) {
    UiKitApp* uiKit = static_cast<UiKitApp*>(app);
    __weak PrUiKitHost* weak = uiKitHostOf(app);
    dispatch_async(dispatch_get_main_queue(), ^{
        PrUiKitHost* host = weak;
        if (host == nil || host.app == nullptr)
            return;
        UiKitAnswer* answer = uiKitAnswerOf(UiKitAnswerKind_Store, id, PrStatus_Ok);
        AppDataStore* store = appDataSourceStore(uiKit->sources, source);
        if (source == kPrAppDataSourceClipboard)
            readPasteboard(kind, answer->store);
        else if (source == kPrAppDataSourceSelection || source == kPrAppDataSourceNone)
            ;
        else if (store == nullptr)
            answer->status = PrStatus_NotFound;
        else
            answer->external = store;
        [host answer:answer];
    });
}

// Returns the path of the file at `index` of a source's files, or nil if it has none.
NSString* PR_NULLABLE filePathOf(UiKitApp* app, PrAppDataSource source, uint32_t index) {
    AppDataStore* store = appDataSourceStore(app->sources, source);
    uint32_t count = 0;
    const PrAppDataFile* files = store != nullptr ? appDataStoreFiles(*store, &count) : nullptr;
    if (index >= count || files[index].path.size == 0 || files[index].directory)
        return nil;
    return uiKitStringOf(files[index].path);
}

// Reads a file's bytes on a queue of its own, and answers from the main queue.
void readFile(PrApp* app, uint32_t id, PrAppDataSource source, uint32_t index, uint64_t offset, uint64_t size) {
    NSString* path = filePathOf(static_cast<UiKitApp*>(app), source, index);
    UiKitAnswer* answer = uiKitAnswerOf(UiKitAnswerKind_Bytes, id, path != nil ? PrStatus_Ok : PrStatus_NotFound);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSFileHandle* file = path != nil ? [NSFileHandle fileHandleForReadingAtPath:path] : nil;
        if (path != nil && file == nil)
            answer->status = PrStatus_Io;
        NSData* bytes = nil;
        if (file != nil && [file seekToOffset:offset error:nil])
            bytes = [file readDataUpToLength:NSUInteger(size) error:nil];
        if (bytes != nil)
            answer->bytes.append(static_cast<const uint8_t*>(bytes.bytes), uint32_t(bytes.length));
        [file closeAndReturnError:nil];
        uiKitAnswerSoon(app, answer);
    });
}

#if !TARGET_OS_TV

// Returns the content types of a dialog's file types, or any item if it names none.
NSArray<UTType*>* contentTypesOf(const PrAppFileDialogDesc& desc) {
    NSMutableArray<UTType*>* types = [NSMutableArray array];
    for (uint32_t i = 0; i < desc.typeCount; ++i) {
        for (NSString* extension in [uiKitStringOf(desc.types[i].extensions) componentsSeparatedByString:@","]) {
            NSString* trimmed = [extension stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
            if (UTType* type = trimmed.length != 0 ? [UTType typeWithFilenameExtension:trimmed] : nil)
                [types addObject:type];
        }
    }
    return types.count != 0 ? types : @[ UTTypeItem ];
}

// Returns the controller that a dialog of `parent` presents from: the parent's, or that of a window that shows.
UIViewController* PR_NULLABLE presenterOf(PrUiKitHost* host, PrAppWindow* PR_NULLABLE parent) {
    PrUiKitWindow* owner = parent != nullptr ? uiKitObjectOf(parent) : nil;
    if (owner.window != nil)
        return owner.controller;
    for (PrUiKitWindow* window in host.windows) {
        if (window.window != nil)
            return window.controller;
    }
    return nil;
}

// Dismisses the dialogs whose requests were canceled, which never answer.
void dismissCanceled(PrUiKitHost* host) {
    UiKitApp* app = static_cast<UiKitApp*>(host.app);
    for (PrUiKitPicker* picker in [host.presented copy]) {
        PrAppDataSource source = kPrAppDataSourceNone;
        AppDataAnswer kind = 0;
        if (appDataRequestsFind(app->requests, picker.request, &source, &kind))
            continue;
        [picker.controller dismissViewControllerAnimated:YES completion:nil];
        [host.presented removeObject:picker];
    }
}

#endif

} // namespace

UiKitAnswer* uiKitAnswerOf(UiKitAnswerKind kind, uint32_t id, PrStatus status) {
    UiKitAnswer* answer = allocZeroed<UiKitAnswer>();
    answer->kind = kind;
    answer->id = id;
    answer->status = status;
    return answer;
}

void uiKitAnswerDestroy(UiKitAnswer* PR_NULLABLE answer) {
    if (answer == nullptr)
        return;
    appDataStoreReset(answer->store);
    answer->bytes.reset();
    prMemoryFree(answer);
}

void uiKitAnswerSoon(PrApp* app, UiKitAnswer* answer) {
    __weak PrUiKitHost* weak = uiKitHostOf(app);
    dispatch_async(dispatch_get_main_queue(), ^{
        PrUiKitHost* host = weak;
        if (host == nil || host.app == nullptr) {
            uiKitAnswerDestroy(answer);
            return;
        }
        [host answer:answer];
    });
}

void uiKitDataRetain(PrApp* app, PrAppDataSource source) {
    appDataSourceRetain(static_cast<UiKitApp*>(app)->sources, source);
}

void uiKitDataRelease(PrApp* app, PrAppDataSource source) {
    appDataSourceRelease(static_cast<UiKitApp*>(app)->sources, source);
}

void uiKitDataRead(PrApp* app, const PrAppDataRead* read) {
    UiKitApp* uiKit = static_cast<UiKitApp*>(app);
    uint32_t id = appDataRequestsAdd(uiKit->requests, read->source, read->kind, read->done, read->context, read->type);
    if (read->kind == PrAppDataReadKind_File)
        readFile(app, id, read->source, read->index, read->offset, read->size);
    else
        readContent(app, id, read->source, read->kind);
}

// Writes the text, HTML, first URL and image of `content` to the general pasteboard, as one item.
PrStatus uiKitDataWrite(PrApp*, PrAppDataSource source, const PrAppDataContent* content) {
#if TARGET_OS_TV
    (void)source;
    (void)content;
    return PrStatus_NotSupported;
#else
    if (source != kPrAppDataSourceClipboard)
        return PrStatus_NotSupported;
    NSMutableDictionary<NSString*, id>* item = [NSMutableDictionary dictionary];
    if ((content->forms & PrAppDataFormFlag_Text) != 0)
        item[UTTypeUTF8PlainText.identifier] = uiKitStringOf(content->text);
    if ((content->forms & PrAppDataFormFlag_Html) != 0)
        item[UTTypeHTML.identifier] = uiKitStringOf(content->html);
    if ((content->forms & PrAppDataFormFlag_Urls) != 0 && content->urlCount != 0) {
        if (NSURL* url = [NSURL URLWithString:uiKitStringOf(content->urls[0])])
            item[UTTypeURL.identifier] = url;
    }
    if ((content->forms & PrAppDataFormFlag_Image) != 0) {
        if (NSData* png = pngOf(content->image))
            item[UTTypePNG.identifier] = png;
    }
    UIPasteboard.generalPasteboard.items = item.count != 0 ? @[ item ] : @[];
    return PrStatus_Ok;
#endif
}

// Writes a file's bytes on a queue of its own, and answers from the main queue. A write at offset zero empties the file
// first.
void uiKitDataWriteFile(PrApp* app, PrAppDataSource source, uint32_t index, uint64_t offset, PrByteView bytes,
        PrAppDataStatusFn done, void* PR_NULLABLE context) {
    UiKitApp* uiKit = static_cast<UiKitApp*>(app);
    uint32_t id = appDataRequestsAdd(uiKit->requests, source, AppDataAnswer_Status, (void (*)(void))done, context);
    NSString* path = filePathOf(uiKit, source, index);
    NSData* data = [NSData dataWithBytes:bytes.data length:bytes.size];
    UiKitAnswer* answer = uiKitAnswerOf(UiKitAnswerKind_Status, id, path != nil ? PrStatus_Ok : PrStatus_NotFound);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        if (path != nil && offset == 0)
            [NSFileManager.defaultManager createFileAtPath:path contents:nil attributes:nil];
        NSFileHandle* file = path != nil ? [NSFileHandle fileHandleForWritingAtPath:path] : nil;
        bool written = file != nil && [file seekToOffset:offset error:nil] && [file writeData:data error:nil];
        if (path != nil && !written)
            answer->status = PrStatus_Io;
        [file closeAndReturnError:nil];
        uiKitAnswerSoon(app, answer);
    });
}

// Opens the system's document picker. The files that the user picks are copies in the application's container, and the
// directories are the user's own, which the application may reach until it ends. A Save dialog answers NotSupported,
// since the picker exports a file that exists, and the program writes its file after the answer.
void uiKitFileDialog(PrApp* app, const PrAppFileDialogDesc* desc, PrAppFileDialogFn done, void* PR_NULLABLE context) {
    PrUiKitHost* host = uiKitHostOf(app);
    uint32_t id = appDataRequestsAdd(static_cast<UiKitApp*>(app)->requests, kPrAppDataSourceNone, AppDataAnswer_Source,
        (void (*)(void))done, context);
#if TARGET_OS_TV
    (void)host;
    (void)desc;
    uiKitAnswerSoon(app, uiKitAnswerOf(UiKitAnswerKind_Source, id, PrStatus_NotSupported));
#else
    UIViewController* presenter = host.headless ? nil : presenterOf(host, desc->parent);
    if (presenter == nil || desc->kind != PrAppFileDialogKind_Open) {
        uiKitAnswerSoon(app, uiKitAnswerOf(UiKitAnswerKind_Source, id, PrStatus_NotSupported));
        return;
    }
    bool directories = (desc->flags & PrAppFileDialogFlag_Directories) != 0;
    NSArray<UTType*>* types = directories ? @[ UTTypeFolder ] : contentTypesOf(*desc);
    UIDocumentPickerViewController* controller =
        [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:types asCopy:!directories];
    controller.allowsMultipleSelection = (desc->flags & PrAppFileDialogFlag_Multiple) != 0;
    if (desc->directory.size != 0)
        controller.directoryURL = [NSURL fileURLWithPath:uiKitStringOf(desc->directory) isDirectory:YES];
    PrUiKitPicker* picker = [[PrUiKitPicker alloc] init];
    picker.app = app;
    picker.request = id;
    picker.directories = directories;
    picker.controller = controller;
    controller.delegate = picker;
    [host.presented addObject:picker];
    [presenter presentViewController:controller animated:YES completion:nil];
#endif
}

void uiKitDataCancel(PrApp* app, void* PR_NULLABLE context) {
    appDataRequestsCancel(static_cast<UiKitApp*>(app)->requests, context);
#if !TARGET_OS_TV
    // A dialog closes on the main thread, which alone holds the dialogs.
    PrUiKitHost* host = uiKitHostOf(app);
    if (NSThread.isMainThread) {
        dismissCanceled(host);
        return;
    }
    __weak PrUiKitHost* weak = host;
    dispatch_async(dispatch_get_main_queue(), ^{
        if (PrUiKitHost* main = weak; main != nil && main.app != nullptr)
            dismissCanceled(main);
    });
#endif
}

// Shows the system's share sheet, which answers once the user picks a service or dismisses it.
void uiKitShare(PrAppWindow* window, const PrAppDataContent* content, PrRect anchor, PrAppShareFn done,
        void* PR_NULLABLE context) {
    PrApp* app = static_cast<UiKitWindow*>(window)->app;
    uint32_t id = appDataRequestsAdd(static_cast<UiKitApp*>(app)->requests, kPrAppDataSourceNone, AppDataAnswer_Status,
        (void (*)(void))done, context);
#if TARGET_OS_TV
    // A television has no share sheet.
    (void)content;
    (void)anchor;
    uiKitAnswerSoon(app, uiKitAnswerOf(UiKitAnswerKind_Status, id, PrStatus_NotSupported));
#else
    NSMutableArray* items = [NSMutableArray array];
    if ((content->forms & PrAppDataFormFlag_Text) != 0)
        [items addObject:uiKitStringOf(content->text)];
    if ((content->forms & PrAppDataFormFlag_Urls) != 0) {
        for (uint32_t i = 0; i < content->urlCount; ++i) {
            if (NSURL* url = [NSURL URLWithString:uiKitStringOf(content->urls[i])])
                [items addObject:url];
        }
    }
    if ((content->forms & PrAppDataFormFlag_Files) != 0) {
        for (uint32_t i = 0; i < content->pathCount; ++i)
            [items addObject:[NSURL fileURLWithPath:uiKitStringOf(content->paths[i])]];
    }
    if ((content->forms & PrAppDataFormFlag_Image) != 0) {
        if (NSData* png = pngOf(content->image))
            [items addObject:[UIImage imageWithData:png]];
    }
    PrUiKitWindow* object = uiKitObjectOf(window);
    if (object.window == nil || items.count == 0) {
        PrStatus status = object.window == nil ? PrStatus_NotSupported : PrStatus_InvalidData;
        uiKitAnswerSoon(app, uiKitAnswerOf(UiKitAnswerKind_Status, id, status));
        return;
    }
    UIActivityViewController* sheet = [[UIActivityViewController alloc] initWithActivityItems:items
        applicationActivities:nil];
    sheet.completionWithItemsHandler = ^(UIActivityType, BOOL, NSArray*, NSError*) {
        uiKitAnswerSoon(app, uiKitAnswerOf(UiKitAnswerKind_Status, id, PrStatus_Ok));
    };
    // On a tablet, the sheet is a popover that points to the anchor.
    sheet.popoverPresentationController.sourceView = object.view;
    sheet.popoverPresentationController.sourceRect = CGRectMake(CGFloat(anchor.left), CGFloat(anchor.top),
        CGFloat(anchor.right - anchor.left), CGFloat(anchor.bottom - anchor.top));
    [object.controller presentViewController:sheet animated:YES completion:nil];
#endif
}

} // namespace pr_impl

using namespace pr_impl;

#if !TARGET_OS_TV

@implementation PrUiKitPicker
// Answers with a source of the picked files' paths, or with None if the user canceled.
- (void)answer:(NSArray<NSURL*>* PR_NULLABLE)urls {
    PrUiKitHost* host = uiKitHostOf(self.app);
    [host.presented removeObject:self];
    if (host.app == nullptr)
        return;
    UiKitAnswer* answer = uiKitAnswerOf(UiKitAnswerKind_Source, self.request, PrStatus_Ok);
    if (urls.count != 0) {
        AppDataStore* store = allocZeroed<AppDataStore>();
        for (NSURL* url in urls) {
            if (self.directories)
                [url startAccessingSecurityScopedResource];
            PrRawString path = uiKitTextOf(url.path);
            appDataStoreAddPath(*store, prRawStringView(&path));
            prRawStringReset(&path);
        }
        answer->source = appDataSourceCreate(static_cast<UiKitApp*>(self.app)->sources, store, nullptr);
    }
    [host answer:answer];
}
- (void)documentPicker:(UIDocumentPickerViewController*)controller didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls {
    (void)controller;
    [self answer:urls];
}
- (void)documentPickerWasCancelled:(UIDocumentPickerViewController*)controller {
    (void)controller;
    [self answer:nil];
}
@end

#endif
