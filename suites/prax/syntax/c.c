// A ring buffer of bytes, with a test that fills and drains it.

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RING_CAPACITY 64u
#define MIN(a, b) ((a) < (b) ? (a) : (b))

typedef struct Ring {
    uint8_t data[RING_CAPACITY];
    size_t head;    // the next byte to read
    size_t count;
} Ring;

enum RingStatus { RING_OK = 0, RING_FULL = -1, RING_EMPTY = -2 };

static size_t ringWrite(Ring* ring, const void* bytes, size_t size) {
    const uint8_t* in = (const uint8_t*)bytes;
    size_t written = MIN(size, RING_CAPACITY - ring->count);
    for (size_t i = 0; i < written; ++i)
        ring->data[(ring->head + ring->count + i) % RING_CAPACITY] = in[i];
    ring->count += written;
    return written;
}

static int ringRead(Ring* ring, uint8_t* out) {
    if (ring->count == 0)
        return RING_EMPTY;
    *out = ring->data[ring->head];
    ring->head = (ring->head + 1) % RING_CAPACITY;
    --ring->count;
    return RING_OK;
}

int main(int argc, char** argv) {
    Ring ring = {.head = 0, .count = 0};
    const char* message = argc > 1 ? argv[1] : "hello,\tring\n";
    size_t written = ringWrite(&ring, message, strlen(message));
    assert(written == strlen(message));

    uint8_t c;
    while (ringRead(&ring, &c) == RING_OK)
        putchar(c == '\t' ? ' ' : c);
    printf("capacity %u, hex 0x%X, ratio %.2f\n", RING_CAPACITY, 255, 1.5e-3);
    return EXIT_SUCCESS;
}
