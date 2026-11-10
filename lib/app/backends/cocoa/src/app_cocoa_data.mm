// The Cocoa backend's data transfer: the pasteboard, drags and drops, reads and writes of sources, and file dialogs.

#include "app/backends/cocoa/src/app_cocoa_data.h"

#include "app/backends/cocoa/src/app_cocoa_input.h"

namespace pr_impl {

namespace {

PrUVec2 imagePixels(NSImage* PR_NULLABLE image, NSMutableData* pixels);

// Returns the image that the pasteboard holds or names, as an image file copied in the Finder does, as straight-alpha
// sRGB Rgba8 rows in `pixels`, and its extent, or a zero extent if it holds none.
PrUVec2 pasteboardPixels(NSPasteboard* board, NSMutableData* pixels) {
    if (![NSImage canInitWithPasteboard:board])
        return PrUVec2{0, 0};
    return imagePixels([[NSImage alloc] initWithPasteboard:board], pixels);
}

// Returns `image` as straight-alpha sRGB Rgba8 rows in `pixels`, and its extent, or a zero extent if there is none.
PrUVec2 imagePixels(NSImage* PR_NULLABLE image, NSMutableData* pixels) {
    CGImageRef source = image != nil ? [image CGImageForProposedRect:nil context:nil hints:nil] : nullptr;
    if (source == nullptr)
        return PrUVec2{0, 0};
    size_t width = CGImageGetWidth(source);
    size_t height = CGImageGetHeight(source);
    pixels.length = width * height * 4;
    uint8_t* rows = static_cast<uint8_t*>(pixels.mutableBytes);
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef context = CGBitmapContextCreate(rows, width, height, 8, width * 4, space,
        kCGImageAlphaPremultipliedLast);
    CGColorSpaceRelease(space);
    if (context == nullptr)
        return PrUVec2{0, 0};
    CGContextSetBlendMode(context, kCGBlendModeCopy);
    CGContextDrawImage(context, CGRectMake(0, 0, CGFloat(width), CGFloat(height)), source);
    CGContextRelease(context);
    // Core Graphics draws only into premultiplied pixels.
    for (size_t i = 0; i < width * height * 4; i += 4) {
        uint32_t alpha = rows[i + 3];
        if (alpha == 0 || alpha == 255)
            continue;
        for (size_t channel = 0; channel < 3; ++channel) {
            uint32_t straight = (rows[i + channel] * 255u + alpha / 2) / alpha;
            rows[i + channel] = uint8_t(straight < 255 ? straight : 255);
        }
    }
    return PrUVec2{uint32_t(width), uint32_t(height)};
}

// The options that read only file URLs from a pasteboard, or every URL.
NSDictionary<NSPasteboardReadingOptionKey, id>* fileOptions() {
    return @{NSPasteboardURLReadingFileURLsOnlyKey: @YES};
}

// Returns true if `type` is a pasteboard type of the system's, which no custom content of a program's has.
bool isSystemType(NSPasteboardType type) {
    for (NSString* prefix in @[ @"public.", @"com.apple.", @"dyn.", @"NS", @"Apple", @"CorePasteboardFlavorType" ]) {
        if ([type hasPrefix:prefix])
            return true;
    }
    return [type isEqualToString:kPayloadType];
}

// Returns the URLs of the pasteboard that do not name files.
NSArray<NSURL*>* webUrlsOf(NSPasteboard* board) {
    NSMutableArray<NSURL*>* urls = [NSMutableArray array];
    for (NSURL* url in [board readObjectsForClasses:@[ NSURL.class ] options:@{}]) {
        if (!url.isFileURL)
            [urls addObject:url];
    }
    return urls;
}

// Reads the content of the pasteboard that `answer` asks for into `out`. A read of forms reads them all but the image
// and the custom content, whose forms the store then names. Text reads a link that holds no text as its URL.
void readPasteboard(NSPasteboard* board, AppDataAnswer answer, PrStringView type, AppDataStore& out) {
    switch (answer) {
        case PrAppDataReadKind_Forms:
            out.forms = cocoaFormsOf(board);
            break;
        case PrAppDataReadKind_Text: {
            NSString* text = [board stringForType:NSPasteboardTypeString];
            if (text == nil)
                text = [NSURL URLFromPasteboard:board].absoluteString;
            PrRawString utf8 = prAppleNSStringToRawString(text != nil ? text : @"");
            appDataStoreSetText(out, prRawStringView(&utf8));
            prRawStringReset(&utf8);
            break;
        }
        case PrAppDataReadKind_Html: {
            NSString* html = [board stringForType:NSPasteboardTypeHTML];
            PrRawString utf8 = prAppleNSStringToRawString(html != nil ? html : @"");
            appDataStoreSetHtml(out, prRawStringView(&utf8));
            prRawStringReset(&utf8);
            break;
        }
        case PrAppDataReadKind_Urls:
            for (NSURL* url in webUrlsOf(board)) {
                PrRawString utf8 = prAppleNSStringToRawString(url.absoluteString);
                appDataStoreAddUrl(out, prRawStringView(&utf8));
                prRawStringReset(&utf8);
            }
            break;
        case PrAppDataReadKind_Files:
            for (NSURL* url in [board readObjectsForClasses:@[ NSURL.class ] options:fileOptions()])
                cocoaAddFile(url, out);
            break;
        case PrAppDataReadKind_Image: {
            NSMutableData* pixels = [NSMutableData data];
            PrUVec2 extent = pasteboardPixels(board, pixels);
            if (extent.x != 0 && extent.y != 0)
                appDataStoreSetImage(out, PrPixelView{
                    static_cast<const uint8_t*>(pixels.bytes), size_t(extent.x) * 4,
                    extent,
                });
            break;
        }
        case PrAppDataReadKind_Custom: {
            NSData* bytes = [board dataForType:cocoaStringOf(type)];
            if (bytes != nil)
                appDataStoreAddCustom(out, type, PrByteView{static_cast<const uint8_t*>(bytes.bytes), bytes.length});
            break;
        }
        default:
            break;
    }
}

// Decodes the image of a drop's store, kept as the custom content of its pasteboard type, into `out`.
void readDropImage(AppDataStore& store, AppDataStore& out) {
    for (NSPasteboardType type in cocoaImageTypes()) {
        PrRawString name = prAppleNSStringToRawString(type);
        PrByteView bytes = appDataStoreCustom(store, prRawStringView(&name));
        prRawStringReset(&name);
        if (bytes.size == 0)
            continue;
        NSData* data = [NSData dataWithBytesNoCopy:const_cast<uint8_t*>(bytes.data) length:bytes.size freeWhenDone:NO];
        NSMutableData* pixels = [NSMutableData data];
        PrUVec2 extent = imagePixels([[NSImage alloc] initWithData:data], pixels);
        if (extent.x != 0 && extent.y != 0)
            appDataStoreSetImage(out, PrPixelView{
                static_cast<const uint8_t*>(pixels.bytes), size_t(extent.x) * 4,
                extent,
            });
        return;
    }
}

// Answers the request `id` of content from the main queue: from the general pasteboard, or from a source's store. A
// source's store does not change, and the request's reference keeps it, so its answer copies only a decoded image.
void readContent(PrApp* app, uint32_t id, PrAppDataSource source, AppDataAnswer kind, PrStringView type) {
    CocoaApp* cocoa = static_cast<CocoaApp*>(app);
    PrRawString custom = prRawStringCopy(type);
    __weak PrCocoaHost* weak = cocoaHostOf(app);
    dispatch_async(dispatch_get_main_queue(), ^{
        PrRawString owned = custom;
        PrCocoaHost* host = weak;
        if (host == nil || host.app == nullptr) {
            prRawStringReset(&owned);
            return;
        }
        AppAnswer* answer = appAnswerCreate(AppAnswerKind_Store, id, PrStatus_Ok);
        AppDataStore* store = appDataSourceStore(cocoa->sources, source);
        if (source == kPrAppDataSourceClipboard)
            readPasteboard(NSPasteboard.generalPasteboard, kind, prRawStringView(&owned), answer->store);
        else if (source == kPrAppDataSourceSelection || source == kPrAppDataSourceNone)
            ;
        else if (store == nullptr)
            answer->status = PrStatus_NotFound;
        else if (kind == PrAppDataReadKind_Image)
            readDropImage(*store, answer->store);
        else
            answer->external = store;
        prRawStringReset(&owned);
        [host answer:answer];
    });
}

// Returns the path of the file at `index` of a source's files, or nil if it has none.
NSString* PR_NULLABLE filePathOf(CocoaApp* app, PrAppDataSource source, uint32_t index) {
    AppDataStore* store = appDataSourceStore(app->sources, source);
    uint32_t count = 0;
    const PrAppDataFile* files = store != nullptr ? appDataStoreFiles(*store, &count) : nullptr;
    if (index >= count || files[index].path.size == 0 || files[index].directory)
        return nil;
    return cocoaStringOf(files[index].path);
}

// Reads a file's bytes on a queue of its own, and answers from the main queue.
void readFile(PrApp* app, uint32_t id, PrAppDataSource source, uint32_t index, uint64_t offset, uint64_t size) {
    CocoaApp* cocoa = static_cast<CocoaApp*>(app);
    NSString* path = filePathOf(cocoa, source, index);
    AppAnswer* answer = appAnswerCreate(AppAnswerKind_Bytes, id, path != nil ? PrStatus_Ok : PrStatus_NotFound);
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
        cocoaAnswerSoon(app, answer);
    });
}

// Returns the content types of the dialog's file types, of each extension that the system knows.
NSArray<UTType*>* contentTypesOf(const PrAppFileDialogDesc* desc) {
    NSMutableArray<UTType*>* types = [NSMutableArray array];
    NSCharacterSet* space = NSCharacterSet.whitespaceCharacterSet;
    for (uint32_t i = 0; i < desc->typeCount; ++i) {
        for (NSString* extension in [cocoaStringOf(desc->types[i].extensions) componentsSeparatedByString:@","]) {
            NSString* trimmed = [extension stringByTrimmingCharactersInSet:space];
            UTType* type = trimmed.length != 0 ? [UTType typeWithFilenameExtension:trimmed] : nil;
            if (type != nil)
                [types addObject:type];
        }
    }
    return types;
}

// Closes the dialogs whose reads were cancelled, whose answers then call nothing.
void closeCancelledDialogs(PrCocoaHost* host) {
    if (host.app == nullptr)
        return;
    AppDataRequests& requests = static_cast<CocoaApp*>(host.app)->requests;
    for (NSNumber* id in host.dialogs.allKeys) {
        PrAppDataSource source = kPrAppDataSourceNone;
        AppDataAnswer answer = AppDataAnswer_Source;
        if (appDataRequestsFind(requests, id.unsignedIntValue, &source, &answer))
            continue;
        NSSavePanel* panel = host.dialogs[id];
        [host.dialogs removeObjectForKey:id];
        if (panel.sheetParent != nil)
            [panel.sheetParent endSheet:panel returnCode:NSModalResponseCancel];
        else
            [panel cancel:nil];
    }
}

// Starts the drag from the main queue once no callback runs, as a modal run loop within a callback may run the queue.
void startDragSoon(PrCocoaWindow* object) {
    __weak PrCocoaWindow* weak = object;
    dispatch_async(dispatch_get_main_queue(), ^{
        PrCocoaWindow* window = weak;
        if (window == nil || window.record == nullptr)
            return;
        if (window.host.inCallback)
            startDragSoon(window);
        else
            [window startDrag];
    });
}

} // namespace

NSArray<NSPasteboardType>* cocoaImageTypes() {
    return @[ NSPasteboardTypePNG, NSPasteboardTypeTIFF ];
}

PrAppDragOperation cocoaOperationOf(NSDragOperation operation) {
    if ((operation & (NSDragOperationMove | NSDragOperationGeneric)) != 0)
        return PrAppDragOperation_Move;
    if ((operation & NSDragOperationCopy) != 0)
        return PrAppDragOperation_Copy;
    if ((operation & NSDragOperationLink) != 0)
        return PrAppDragOperation_Link;
    return PrAppDragOperation_None;
}

NSBitmapImageRep* PR_NULLABLE cocoaImageRepOf(PrPixelView pixels) {
    NSBitmapImageRep* rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr
        pixelsWide:NSInteger(pixels.extent.x) pixelsHigh:NSInteger(pixels.extent.y) bitsPerSample:8 samplesPerPixel:4
        hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bitmapFormat:NSBitmapFormatAlphaNonpremultiplied
        bytesPerRow:NSInteger(pixels.extent.x) * 4 bitsPerPixel:32];
    if (rep == nil)
        return nil;
    for (uint32_t y = 0; y < pixels.extent.y; ++y)
        prMemCopy(rep.bitmapData + size_t(y) * size_t(rep.bytesPerRow), pixels.data + y * pixels.stride,
            size_t(pixels.extent.x) * 4);
    return [rep bitmapImageRepByRetaggingWithColorSpace:NSColorSpace.sRGBColorSpace];
}

PrAppDragOperationFlags cocoaOperationsOf(NSDragOperation mask) {
    PrAppDragOperationFlags operations = 0;
    if ((mask & NSDragOperationCopy) != 0)
        operations |= PrAppDragOperationFlag_Copy;
    if ((mask & (NSDragOperationMove | NSDragOperationGeneric)) != 0)
        operations |= PrAppDragOperationFlag_Move;
    if ((mask & NSDragOperationLink) != 0)
        operations |= PrAppDragOperationFlag_Link;
    return operations;
}

NSDragOperation cocoaMaskOf(PrAppDragOperationFlags operations) {
    NSDragOperation mask = NSDragOperationNone;
    if ((operations & PrAppDragOperationFlag_Copy) != 0)
        mask |= NSDragOperationCopy;
    if ((operations & PrAppDragOperationFlag_Move) != 0)
        mask |= NSDragOperationMove | NSDragOperationGeneric;
    if ((operations & PrAppDragOperationFlag_Link) != 0)
        mask |= NSDragOperationLink;
    return mask;
}

NSDragOperation cocoaAnswerOf(PrAppDragOperation operation, NSDragOperation mask) {
    switch (operation) {
        case PrAppDragOperation_Copy: return NSDragOperationCopy;
        case PrAppDragOperation_Move:
            return (mask & NSDragOperationMove) != 0 ? NSDragOperationMove : NSDragOperationGeneric;
        case PrAppDragOperation_Link: return NSDragOperationLink;
        default: return NSDragOperationNone;
    }
}

PrAppDataFormFlags cocoaFormsOf(NSPasteboard* board) {
    PrAppDataFormFlags forms = 0;
    if ([board availableTypeFromArray:@[ NSPasteboardTypeString ]] != nil)
        forms |= PrAppDataFormFlag_Text;
    if ([board availableTypeFromArray:@[ NSPasteboardTypeHTML ]] != nil)
        forms |= PrAppDataFormFlag_Html;
    if (webUrlsOf(board).count != 0)
        forms |= PrAppDataFormFlag_Urls;
    if ([board canReadObjectForClasses:@[ NSURL.class ] options:fileOptions()])
        forms |= PrAppDataFormFlag_Files;
    if ([board availableTypeFromArray:cocoaImageTypes()] != nil)
        forms |= PrAppDataFormFlag_Image;
    for (NSPasteboardType type in board.types) {
        if (!isSystemType(type))
            forms |= PrAppDataFormFlag_Custom;
    }
    return forms;
}

void cocoaAddFile(NSURL* url, AppDataStore& out) {
    NSNumber* size = nil;
    NSNumber* directory = nil;
    [url getResourceValue:&size forKey:NSURLFileSizeKey error:nil];
    [url getResourceValue:&directory forKey:NSURLIsDirectoryKey error:nil];
    PrRawString name = prAppleNSStringToRawString(url.lastPathComponent);
    appDataStoreAddFile(out, PrAppDataFile{
        .name = prRawStringView(&name),
        .path = url.fileSystemRepresentation,
        .size = size.unsignedLongLongValue,
        .directory = directory.boolValue,
    });
    prRawStringReset(&name);
}

void cocoaReadDrop(NSPasteboard* board, AppDataStore& out) {
    PrAppDataFormFlags forms = cocoaFormsOf(board);
    // The forms that the drop reads now, each with the answer that reads it.
    struct Read {
        PrAppDataFormFlags form;
        AppDataAnswer answer;
    };
    constexpr Read kReads[] = {
        {PrAppDataFormFlag_Text, PrAppDataReadKind_Text},
        {PrAppDataFormFlag_Html, PrAppDataReadKind_Html},
        {PrAppDataFormFlag_Urls, PrAppDataReadKind_Urls},
        {PrAppDataFormFlag_Files, PrAppDataReadKind_Files},
    };
    for (const Read& read : kReads) {
        if ((forms & read.form) != 0)
            readPasteboard(board, read.answer, {}, out);
    }
    for (NSPasteboardType type in board.types) {
        bool image = [cocoaImageTypes() containsObject:type];
        if (!image && isSystemType(type))
            continue;
        NSData* bytes = [board dataForType:type];
        if (bytes != nil) {
            PrRawString name = prAppleNSStringToRawString(type);
            appDataStoreAddCustom(out, prRawStringView(&name),
                PrByteView{static_cast<const uint8_t*>(bytes.bytes), bytes.length});
            prRawStringReset(&name);
        }
    }
    // The image shows as a form of its own, and not as custom content.
    out.forms = forms;
}

void cocoaWriteContent(const PrAppDataContent& content, uint64_t token, NSPasteboardItem* first,
        NSMutableArray<id<NSPasteboardWriting>>* writers) {
    if ((content.forms & PrAppDataFormFlag_Text) != 0)
        [first setString:cocoaStringOf(content.text) forType:NSPasteboardTypeString];
    if ((content.forms & PrAppDataFormFlag_Html) != 0)
        [first setString:cocoaStringOf(content.html) forType:NSPasteboardTypeHTML];
    if ((content.forms & PrAppDataFormFlag_Urls) != 0 && content.urlCount != 0)
        [first setString:cocoaStringOf(content.urls[0]) forType:NSPasteboardTypeURL];
    if ((content.forms & PrAppDataFormFlag_Image) != 0 && content.image.data != nullptr) {
        NSBitmapImageRep* rep = cocoaImageRepOf(content.image);
        NSData* png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
        if (png != nil)
            [first setData:png forType:NSPasteboardTypePNG];
        if (rep.TIFFRepresentation != nil)
            [first setData:rep.TIFFRepresentation forType:NSPasteboardTypeTIFF];
    }
    for (uint32_t i = 0; (content.forms & PrAppDataFormFlag_Custom) != 0 && i < content.customCount; ++i) {
        const PrAppDataCustom& custom = content.custom[i];
        NSData* bytes = [NSData dataWithBytes:custom.bytes.data length:custom.bytes.size];
        [first setData:bytes forType:cocoaStringOf(custom.type)];
    }
    if ((content.forms & PrAppDataFormFlag_Payload) != 0 && token != 0)
        [first setData:[NSData dataWithBytes:&token length:sizeof(token)] forType:kPayloadType];
    for (uint32_t i = 0; (content.forms & PrAppDataFormFlag_Files) != 0 && i < content.pathCount; ++i) {
        NSString* url = [NSURL fileURLWithPath:cocoaStringOf(content.paths[i])].absoluteString;
        NSPasteboardItem* item = i == 0 ? first : [[NSPasteboardItem alloc] init];
        [item setString:url forType:NSPasteboardTypeFileURL];
        if (i != 0)
            [writers addObject:item];
    }
}

bool cocoaCarriesDrag(NSPasteboard* board, const CocoaDrag& drag) {
    NSData* data = [board dataForType:kPayloadType];
    uint64_t token = 0;
    if (!drag.active || data.length != sizeof(token))
        return false;
    [data getBytes:&token length:sizeof(token)];
    return token == drag.token;
}

void cocoaAnswerSoon(PrApp* app, AppAnswer* answer) {
    __weak PrCocoaHost* weak = cocoaHostOf(app);
    dispatch_async(dispatch_get_main_queue(), ^{
        PrCocoaHost* host = weak;
        if (host == nil || host.app == nullptr) {
            appAnswerDestroy(answer, nullptr);
            return;
        }
        [host answer:answer];
    });
}

void cocoaDataRetain(PrApp* app, PrAppDataSource source) {
    appDataSourceRetain(static_cast<CocoaApp*>(app)->sources, source);
}

void cocoaDataRelease(PrApp* app, PrAppDataSource source) {
    appDataSourceRelease(static_cast<CocoaApp*>(app)->sources, source);
}

void cocoaDataRead(PrApp* app, const PrAppDataRead* read) {
    CocoaApp* cocoa = static_cast<CocoaApp*>(app);
    AppDataAnswer kind = read->kind;
    uint32_t id = appDataRequestsAdd(cocoa->requests, read->source, kind, read->done, read->context, read->type);
    if (read->kind == PrAppDataReadKind_File)
        readFile(app, id, read->source, read->index, read->offset, read->size);
    else
        readContent(app, id, read->source, kind, read->type);
}

PrStatus cocoaDataWrite(PrApp* app, PrAppDataSource source, const PrAppDataContent* content) {
    (void)app;
    if (source == kPrAppDataSourceSelection)
        return PrStatus_NotSupported;
    NSPasteboardItem* first = [[NSPasteboardItem alloc] init];
    NSMutableArray<id<NSPasteboardWriting>>* writers = [NSMutableArray arrayWithObject:first];
    cocoaWriteContent(*content, 0, first, writers);
    NSPasteboard* board = NSPasteboard.generalPasteboard;
    [board clearContents];
    return [board writeObjects:writers] ? PrStatus_Ok : PrStatus_Io;
}

void cocoaDataWriteFile(PrApp* app, PrAppDataSource source, uint32_t index, uint64_t offset, PrByteView bytes,
        PrAppDataStatusFn done, void* PR_NULLABLE context) {
    CocoaApp* cocoa = static_cast<CocoaApp*>(app);
    uint32_t id = appDataRequestsAdd(cocoa->requests, source, AppDataAnswer_Status, (void (*)(void))done, context);
    NSString* path = filePathOf(cocoa, source, index);
    NSData* data = [NSData dataWithBytes:bytes.data length:bytes.size];
    AppAnswer* answer = appAnswerCreate(AppAnswerKind_Status, id, path != nil ? PrStatus_Ok : PrStatus_NotFound);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        if (path != nil && offset == 0)
            [NSFileManager.defaultManager createFileAtPath:path contents:nil attributes:nil];
        NSFileHandle* file = path != nil ? [NSFileHandle fileHandleForWritingAtPath:path] : nil;
        bool written = file != nil && [file seekToOffset:offset error:nil] && [file writeData:data error:nil];
        if (path != nil && !written)
            answer->status = PrStatus_Io;
        [file closeAndReturnError:nil];
        cocoaAnswerSoon(app, answer);
    });
}

void cocoaDataCancel(PrApp* app, void* PR_NULLABLE context) {
    appDataRequestsCancel(static_cast<CocoaApp*>(app)->requests, context);
    // A dialog closes on the main thread, which alone holds the dialogs.
    PrCocoaHost* host = cocoaHostOf(app);
    if (NSThread.isMainThread) {
        closeCancelledDialogs(host);
        return;
    }
    __weak PrCocoaHost* weak = host;
    dispatch_async(dispatch_get_main_queue(), ^{
        if (PrCocoaHost* main = weak)
            closeCancelledDialogs(main);
    });
}

void cocoaFileDialog(PrApp* app, const PrAppFileDialogDesc* desc, PrAppFileDialogFn done, void* PR_NULLABLE context) {
    CocoaApp* cocoa = static_cast<CocoaApp*>(app);
    PrCocoaHost* host = cocoaHostOf(app);
    uint32_t id = appDataRequestsAdd(cocoa->requests, kPrAppDataSourceNone, AppDataAnswer_Source, (void (*)(void))done, context);
    if (host.headless || !host.running) {
        cocoaAnswerSoon(app, appAnswerCreate(AppAnswerKind_Source, id, PrStatus_NotSupported));
        return;
    }
    bool directories = desc->kind == PrAppFileDialogKind_Open && (desc->flags & PrAppFileDialogFlag_Directories) != 0;
    NSSavePanel* panel = nil;
    if (desc->kind == PrAppFileDialogKind_Open) {
        NSOpenPanel* open = [NSOpenPanel openPanel];
        open.allowsMultipleSelection = (desc->flags & PrAppFileDialogFlag_Multiple) != 0;
        open.canChooseDirectories = directories;
        open.canChooseFiles = !directories;
        panel = open;
    } else {
        panel = [NSSavePanel savePanel];
        panel.canCreateDirectories = YES;
        if (desc->name.size != 0)
            panel.nameFieldStringValue = cocoaStringOf(desc->name);
    }
    // A sheet shows its message, and a panel of its own shows its title.
    if (desc->title.size != 0) {
        panel.title = cocoaStringOf(desc->title);
        panel.message = cocoaStringOf(desc->title);
    }
    if (desc->directory.size != 0)
        panel.directoryURL = [NSURL fileURLWithPath:cocoaStringOf(desc->directory) isDirectory:YES];
    NSArray<UTType*>* types = contentTypesOf(desc);
    if (types.count != 0 && !directories)
        panel.allowedContentTypes = types;
    if (host.dialogs == nil)
        host.dialogs = [NSMutableDictionary dictionary];
    host.dialogs[@(id)] = panel;
    __weak PrCocoaHost* weak = host;
    __weak NSSavePanel* weakPanel = panel;
    void (^finished)(NSModalResponse) = ^(NSModalResponse response) {
        PrCocoaHost* answering = weak;
        NSSavePanel* chosen = weakPanel;
        if (answering == nil || answering.app == nullptr)
            return;
        [answering.dialogs removeObjectForKey:@(id)];
        // The user's choice is a source of its files, which the answer holds a reference to. A cancel answers None.
        AppAnswer* answer = appAnswerCreate(AppAnswerKind_Source, id, PrStatus_Ok);
        if (response == NSModalResponseOK && chosen != nil) {
            NSArray<NSURL*>* urls = @[ chosen.URL ];
            if ([chosen isKindOfClass:NSOpenPanel.class])
                urls = ((NSOpenPanel*)chosen).URLs;
            AppDataStore* store = allocZeroed<AppDataStore>();
            for (NSURL* url in urls)
                cocoaAddFile(url, *store);
            answer->source = appDataSourceCreate(static_cast<CocoaApp*>(answering.app)->sources, store, nullptr);
        }
        [answering answer:answer];
    };
    NSWindow* parent = desc->parent != nullptr ? cocoaObjectOf(desc->parent).window : nil;
    if (parent != nil && parent.isVisible && parent.attachedSheet == nil) {
        [panel beginSheetModalForWindow:parent completionHandler:finished];
    } else {
        [NSApp activate];
        [panel beginWithCompletionHandler:finished];
    }
}

void cocoaDragPromiseEnd(PrAppWindow* window, uint32_t index, PrStatus status) {
    [cocoaObjectOf(window) endPromise:index status:status];
}

void cocoaDragStart(PrAppWindow* window, const PrAppDragDesc* desc) {
    appDragPendingSet(static_cast<CocoaWindow*>(window)->drag, *desc);
    startDragSoon(cocoaObjectOf(window));
}

} // namespace pr_impl

using namespace pr_impl;

@implementation PrCocoaPromise
- (NSString*)filePromiseProvider:(NSFilePromiseProvider*)filePromiseProvider fileNameForType:(NSString*)fileType {
    (void)filePromiseProvider;
    (void)fileType;
    return self.name;
}
// AppKit asks on the main queue, the provider's default.
- (void)filePromiseProvider:(NSFilePromiseProvider*)filePromiseProvider writePromiseToURL:(NSURL*)url
        completionHandler:(void (^)(NSError* PR_NULLABLE))completionHandler {
    (void)filePromiseProvider;
    PrCocoaWindow* owner = self.owner;
    if (owner == nil) {
        completionHandler([NSError errorWithDomain:NSCocoaErrorDomain code:NSUserCancelledError userInfo:nil]);
        return;
    }
    [owner promise:self.index to:url done:completionHandler];
}
@end
