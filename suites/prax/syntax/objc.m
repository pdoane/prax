// A download queue that runs a few transfers at once and reports each to its delegate.
#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

static const NSUInteger kMaxConcurrent = 3;

@protocol DownloadQueueDelegate <NSObject>
- (void)queue:(id)queue didFinishURL:(NSURL *)url data:(nullable NSData *)data error:(nullable NSError *)error;
@optional
- (void)queueDidDrain:(id)queue;
@end

@interface DownloadQueue : NSObject
@property (nonatomic, weak, nullable) id<DownloadQueueDelegate> delegate;
@property (nonatomic, readonly) NSUInteger pending;
- (void)enqueue:(NSURL *)url;
@end

@implementation DownloadQueue {
    NSOperationQueue *_queue;
    NSURLSession *_session;
}

- (instancetype)init {
    if ((self = [super init])) {
        _queue = [[NSOperationQueue alloc] init];
        _queue.maxConcurrentOperationCount = kMaxConcurrent;
        _session = [NSURLSession sessionWithConfiguration:NSURLSessionConfiguration.defaultSessionConfiguration];
    }
    return self;
}

- (NSUInteger)pending {
    return _queue.operationCount;
}

- (void)enqueue:(NSURL *)url {
    __weak typeof(self) weakSelf = self;
    [_queue addOperationWithBlock:^{
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        [[self->_session dataTaskWithURL:url completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
            [weakSelf.delegate queue:weakSelf didFinishURL:url data:data error:error];
            dispatch_semaphore_signal(done);
        }] resume];
        dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
    }];
}

@end

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        DownloadQueue *queue = [[DownloadQueue alloc] init];
        [queue enqueue:[NSURL URLWithString:@"https://example.com/a.json"]];
        NSLog(@"pending: %lu, max %lu\n", (unsigned long)queue.pending, (unsigned long)kMaxConcurrent);
    }
    return 0;
}

NS_ASSUME_NONNULL_END
