// Decoded audio: a queue of interleaved samples from a decoding thread to a reader on an output's real-time thread,
// which codecs fill for their decoders' audio.
//
// The queue has one writer and one reader, and any thread may ask the writer to seek. Between a request and the
// writer's prMediaAudioQueueBegin, reads return nothing, so that no sample from before the seek plays after it. The
// reader never waits, and the writer waits only while the queue is full.

#pragma once

#include "pr/base.h"
#include "pr/media.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A queue of decoded audio, of one sample rate and channel count.
typedef struct PrMediaAudioQueue PrMediaAudioQueue;

// A request that the writer start again from `seconds`. Each request has a new `id`.
typedef struct PrMediaAudioSeek {
    uint32_t id;
    double seconds;
} PrMediaAudioSeek;

// Creates a queue of samples of `sampleRate` and `channels`, which holds two seconds of them, with a first request to
// start from `seconds`.
PrMediaAudioQueue* prMediaAudioQueueCreate(uint32_t sampleRate, uint32_t channels, double seconds);

// Frees the queue. Neither its writer nor its reader may be using it.
void prMediaAudioQueueDestroy(PrMediaAudioQueue* PR_NULLABLE queue);

// The sample rate and channels of the queue's samples.
uint32_t prMediaAudioQueueSampleRate(const PrMediaAudioQueue* queue);
uint32_t prMediaAudioQueueChannels(const PrMediaAudioQueue* queue);

// Asks the writer to start again from `seconds`. Any thread may call this.
void prMediaAudioQueueSeek(PrMediaAudioQueue* queue, double seconds);

// Asks the writer to stop, so that its thread can be joined. Returns true once asked. Any thread may call these.
void prMediaAudioQueueStop(PrMediaAudioQueue* queue);
bool prMediaAudioQueueIsStopping(const PrMediaAudioQueue* queue);

// The writer's side. The latest request, which the writer serves.
PrMediaAudioSeek prMediaAudioQueueRequest(const PrMediaAudioQueue* queue);

// Returns true if the writer should stop serving the request `id`: a later request was made, or the queue is stopping.
bool prMediaAudioQueueIsInterrupted(const PrMediaAudioQueue* queue, uint32_t id);

// Says that the samples written from now on serve the request `id`, and begin at `time` on the timeline. Reads then
// return them.
void prMediaAudioQueueBegin(PrMediaAudioQueue* queue, uint32_t id, double time);

// Appends `frames` frames, waiting while the queue is full. Returns false if the writer was interrupted before all were
// written.
bool prMediaAudioQueueWrite(PrMediaAudioQueue* queue, uint32_t id, const float* samples, uint32_t frames);

// Says that the writer has written the last frame for the request `id`, and waits for the next request or for the queue
// to stop.
void prMediaAudioQueueFinish(PrMediaAudioQueue* queue, uint32_t id);

// The reader's side. Writes up to `frames` frames to `samples`. Never waits or allocates, so that a real-time thread
// may call it.
PrMediaAudioChunk prMediaAudioQueueRead(PrMediaAudioQueue* queue, float* samples, uint32_t frames);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
