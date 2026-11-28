package io.fastdb.payload;

import java.nio.ByteBuffer;
import java.nio.ReadOnlyBufferException;

/** Checked immutable view over backed or materialized Core data. */
public final class View extends NativeHandle {
    View() {
        super(FastDbNative::viewRelease);
    }

    public synchronized View retain() {
        View result = new View();
        result.adopt(FastDbNative.viewRetain(address()));
        return result;
    }

    public synchronized void requireSpecSha256(byte[] expected) {
        FastDbNative.viewRequireDigest(address(), NativeValues.digest(expected));
    }

    public synchronized ViewKind kind() {
        return ViewKind.fromCode(FastDbNative.viewKind(address()));
    }

    public synchronized boolean isNull() {
        return FastDbNative.viewIsNull(address());
    }

    public synchronized long length() {
        return FastDbNative.viewLength(address());
    }

    public synchronized int componentIndex() {
        return FastDbNative.viewComponentIndex(address());
    }

    public synchronized int fieldCount() {
        return FastDbNative.viewFieldCount(address());
    }

    public synchronized boolean getBool() {
        return FastDbNative.viewGetBool(address());
    }

    public synchronized int getU8() {
        return FastDbNative.viewGetU8(address());
    }

    public synchronized int getU16() {
        return FastDbNative.viewGetU16(address());
    }

    public synchronized long getU32() {
        return FastDbNative.viewGetU32(address());
    }

    public synchronized int getI32() {
        return FastDbNative.viewGetI32(address());
    }

    public synchronized double getU8n() {
        return Double.longBitsToDouble(FastDbNative.viewGetU8n(address()));
    }

    public synchronized double getU16n() {
        return Double.longBitsToDouble(FastDbNative.viewGetU16n(address()));
    }

    public synchronized float getF32() {
        return Float.intBitsToFloat(FastDbNative.viewGetF32(address()));
    }

    public synchronized double getF64() {
        return Double.longBitsToDouble(FastDbNative.viewGetF64(address()));
    }

    public synchronized View at(long index) {
        View result = new View();
        result.adopt(FastDbNative.viewAt(address(), index));
        return result;
    }

    public synchronized View field(int index) {
        View result = new View();
        result.adopt(FastDbNative.viewField(address(), index));
        return result;
    }

    public synchronized View refTarget() {
        View result = new View();
        result.adopt(FastDbNative.viewRefTarget(address()));
        return result;
    }

    public synchronized View materialize() {
        View result = new View();
        result.adopt(FastDbNative.viewMaterialize(address()));
        return result;
    }

    public synchronized GraphIdentity graphIdentity() {
        long[] raw = FastDbNative.viewGraphIdentity(address());
        return new GraphIdentity((int) raw[0], raw[1]);
    }

    public String getStr() {
        try (Access access = acquireAs(ViewKind.STR.code())) {
            return access.asStr();
        }
    }

    public String getWstr() {
        try (Access access = acquireAs(ViewKind.WSTR.code())) {
            return access.asWstr();
        }
    }

    public byte[] getBytes() {
        try (Access access = acquireAs(ViewKind.BYTES.code())) {
            return access.toByteArray();
        }
    }

    /** Copies into the buffer at its current position and advances it by the byte count on success. */
    public synchronized void copyBytesInto(ByteBuffer destination) {
        if (destination.isReadOnly()) {
            throw new ReadOnlyBufferException();
        }
        try (Access access = acquireAs(ViewKind.BYTES.code())) {
            access.copyTo(destination);
        }
    }

    /** Pins a str, wstr or bytes value. Close the pin before invalidating its payload on this thread. */
    public synchronized Access acquire() {
        return acquireAs(FastDbNative.viewKind(address()));
    }

    private synchronized Access acquireAs(int kind) {
        Access result = new Access(kind);
        result.adopt(FastDbNative.viewAcquire(address()));
        return result;
    }
}
