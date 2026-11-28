package io.fastdb.payload;

import java.util.concurrent.atomic.AtomicLong;
import java.util.function.LongConsumer;

abstract class NativeHandle implements AutoCloseable {
    private final AtomicLong address = new AtomicLong();
    private final LongConsumer release;

    NativeHandle(LongConsumer release) {
        this.release = release;
    }

    // Called once before publication. Allocate the Java wrapper before
    // entering JNI so a failed Java allocation cannot strand a native owner.
    final void adopt(long ownedAddress) {
        if (ownedAddress == 0 || !address.compareAndSet(0, ownedAddress)) {
            throw new IllegalStateException("Expected a new FastDB handle");
        }
    }

    // When a wrapper has been published, callers hold its monitor until the
    // JNI call ends or they have acquired an independent native reference.
    final long address() {
        long current = address.get();
        if (current == 0) {
            throw new IllegalStateException("FastDB handle is closed");
        }
        return current;
    }

    @Override
    public final void close() {
        long current;
        synchronized (this) {
            current = address.get();
            if (current == 0 || !address.compareAndSet(current, 0)) {
                return;
            }
        }
        release.accept(current);
    }
}
