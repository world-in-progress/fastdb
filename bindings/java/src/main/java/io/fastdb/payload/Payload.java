package io.fastdb.payload;

import java.nio.ByteBuffer;

/** Immutable payload owner with an explicit invalidation barrier. */
public final class Payload extends NativeHandle {
    Payload() {
        super(FastDbNative::payloadRelease);
    }

    public static Payload openCopy(CompiledSpec spec, byte[] bytes) {
        return openCopy(spec, bytes, null);
    }

    public static Payload openCopy(CompiledSpec spec, byte[] bytes, OpenOptions options) {
        return openArray(spec, bytes, 0, bytes.length, options);
    }

    private static Payload openArray(CompiledSpec spec, byte[] bytes, int offset, int length, OpenOptions options) {
        Payload result = new Payload();
        synchronized (spec) {
            result.adopt(FastDbNative.payloadOpenArray(spec.address(), bytes, offset, length, toNative(options)));
        }
        return result;
    }

    public static Payload openCopy(CompiledSpec spec, ByteBuffer bytes) {
        return openCopy(spec, bytes, null);
    }

    /** Opens a copy of the remaining bytes without advancing the input buffer. */
    public static Payload openCopy(CompiledSpec spec, ByteBuffer bytes, OpenOptions options) {
        if (bytes.isDirect()) {
            return openDirect(spec, bytes, options, false);
        }
        if (bytes.hasArray()) {
            return openArray(spec, bytes.array(), bytes.arrayOffset() + bytes.position(), bytes.remaining(), options);
        }
        return openCopy(spec, NativeValues.copyRemaining(bytes), options);
    }

    public static Payload openExternal(CompiledSpec spec, ByteBuffer directBuffer) {
        return openExternal(spec, directBuffer, null);
    }

    /**
     * Pins and opens the remaining bytes of a direct buffer. The selected bytes
     * must stay unchanged until invalidation or release of the backing.
     */
    public static Payload openExternal(CompiledSpec spec, ByteBuffer directBuffer, OpenOptions options) {
        if (!directBuffer.isDirect()) {
            throw new IllegalArgumentException("External backing requires a direct ByteBuffer");
        }
        return openDirect(spec, directBuffer, options, true);
    }

    private static Payload openDirect(CompiledSpec spec, ByteBuffer bytes, OpenOptions options, boolean external) {
        Payload result = new Payload();
        synchronized (spec) {
            result.adopt(FastDbNative.payloadOpenDirect(spec.address(), bytes, bytes.position(), bytes.remaining(),
                    toNative(options), external));
        }
        return result;
    }

    public static Payload openExternalNative(CompiledSpec spec, NativeBytesBacking backing) {
        return openExternalNative(spec, backing, null);
    }

    public static Payload openExternalNative(CompiledSpec spec, NativeBytesBacking backing, OpenOptions options) {
        Payload result = new Payload();
        synchronized (spec) {
            result.adopt(FastDbNative.payloadOpenNative(spec.address(), backing.dataPtr(), backing.byteLen(),
                    backing.retainFnPtr(), backing.releaseFnPtr(), backing.ownerToken(), toNative(options)));
        }
        return result;
    }

    private static long[] toNative(OpenOptions options) {
        return options == null ? null : options.toNative();
    }

    public synchronized Payload retain() {
        Payload result = new Payload();
        result.adopt(FastDbNative.payloadRetain(address()));
        return result;
    }

    public synchronized void requireSpecSha256(byte[] expected) {
        FastDbNative.payloadRequireDigest(address(), NativeValues.digest(expected));
    }

    public synchronized byte[] sha256() {
        return FastDbNative.payloadSha256(address());
    }

    public synchronized Profile profile() {
        return Profile.fromCode(FastDbNative.payloadProfile(address()));
    }

    public synchronized ExecutionReport executionReport() {
        return ExecutionReport.fromNative(FastDbNative.payloadReport(address()));
    }

    public byte[] binaryBytes() {
        try (Access access = acquire()) {
            return access.toByteArray();
        }
    }

    public synchronized Access acquire() {
        Access result = new Access(0);
        result.adopt(FastDbNative.payloadAcquire(address()));
        return result;
    }

    public synchronized View entryView(int index) {
        View result = new View();
        result.adopt(FastDbNative.payloadEntryView(address(), index));
        return result;
    }

    /** Rejects new backing reads and waits for open access pins to close. */
    public void invalidate() {
        // Do not hold this wrapper's monitor while the Core barrier drains.
        try (Payload live = retain()) {
            FastDbNative.payloadInvalidate(live.address());
        }
    }
}
