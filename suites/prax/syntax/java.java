// A rate limiter over a token bucket, safe for many threads.
package com.example.limits;

import java.time.Clock;
import java.time.Duration;
import java.util.Objects;
import java.util.concurrent.atomic.AtomicLong;

/**
 * Admits up to {@code capacity} requests at once, refilled at {@code rate} per second.
 */
public final class TokenBucket implements AutoCloseable {
    private static final long NANOS_PER_SECOND = 1_000_000_000L;

    private final long capacity;
    private final double rate;
    private final Clock clock;
    private final AtomicLong tokens;
    private volatile long lastRefill;

    public TokenBucket(long capacity, double rate, Clock clock) {
        if (capacity <= 0 || rate <= 0.0) {
            throw new IllegalArgumentException("capacity and rate must be positive: " + capacity + ", " + rate);
        }
        this.capacity = capacity;
        this.rate = rate;
        this.clock = Objects.requireNonNull(clock);
        this.tokens = new AtomicLong(capacity);
        this.lastRefill = System.nanoTime();
    }

    public boolean tryAcquire() {
        refill();
        return tokens.getAndUpdate(t -> t > 0 ? t - 1 : t) > 0;
    }

    private synchronized void refill() {
        long now = System.nanoTime();
        long added = (long) ((now - lastRefill) * rate / NANOS_PER_SECOND);
        if (added > 0) {
            tokens.updateAndGet(t -> Math.min(capacity, t + added));
            lastRefill = now;
        }
    }

    @Override
    public void close() {
        tokens.set(0);
    }

    public static void main(String[] args) throws InterruptedException {
        try (var bucket = new TokenBucket(2, 1.5, Clock.systemUTC())) {
            for (int i = 0; i < 5; i++) {
                System.out.printf("request %d: %s%n", i, bucket.tryAcquire() ? "admitted" : "limited");
                Thread.sleep(Duration.ofMillis(250));
            }
        }
    }
}
