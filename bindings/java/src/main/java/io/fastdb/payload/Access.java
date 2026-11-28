package io.fastdb.payload;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;

/** Unique, scoped pin on a payload image or a variable-width value. */
public final class Access extends NativeHandle {
    private final int kind;

    Access(int kind) {
        super(FastDbNative::accessRelease);
        this.kind = kind;
    }

    public synchronized long byteLength() {
        return FastDbNative.accessByteLength(address(), kind);
    }

    /** Returns a copy. For wstr the bytes represent its native-order UTF-16 code units. */
    public synchronized byte[] toByteArray() {
        return FastDbNative.accessToArray(address(), kind);
    }

    /** Copies into the remaining destination and advances its position on success. */
    public synchronized void copyTo(ByteBuffer destination) {
        NativeValues.copyInto(address(), kind, false, destination);
    }

    /**
     * Returns a read-only alias in native byte order. The alias and its
     * derivatives may be used only while this Access is open. The alias
     * does not pin backing or keep this Access alive.
     */
    public synchronized ByteBuffer unsafeByteBuffer() {
        return FastDbNative.accessDirectBuffer(address(), kind).asReadOnlyBuffer().order(ByteOrder.nativeOrder());
    }

    public synchronized String asStr() {
        return NativeValues.text(FastDbNative.accessToArray(address(), ViewKind.STR.code()));
    }

    public synchronized String asWstr() {
        return FastDbNative.accessWstr(address());
    }

    /** Borrows the native address for this pin's lifetime. The address is read-only. */
    public synchronized long dataAddress() {
        return FastDbNative.accessDataAddress(address(), kind);
    }
}
