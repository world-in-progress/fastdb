package io.fastdb.payload;

import java.nio.ByteBuffer;
import java.util.Objects;

/** Mutable, thread-confined authoring state. A successful freeze seals it. */
public final class Builder extends NativeHandle {
    private Builder() {
        super(FastDbNative::builderRelease);
    }

    public static Builder create(CompiledSpec spec) {
        return create(spec, null);
    }

    public static Builder create(CompiledSpec spec, BuilderOptions options) {
        Builder result = new Builder();
        synchronized (spec) {
            result.adopt(FastDbNative.builderCreate(spec.address(), options == null ? null : options.toNative()));
        }
        return result;
    }

    public synchronized void requireSpecSha256(byte[] expected) {
        FastDbNative.builderRequireDigest(address(), NativeValues.digest(expected));
    }

    public synchronized Builder entryBegin(int index, long valueCount) {
        NativeValues.requireNonNegative(valueCount);
        FastDbNative.builderEntryBegin(address(), index, valueCount);
        return this;
    }

    public synchronized ObjectHandle declareObject(int componentIndex) {
        return new ObjectHandle(FastDbNative.builderDeclareObject(address(), componentIndex));
    }

    public synchronized Builder valueNull() {
        FastDbNative.builderValueNull(address());
        return this;
    }

    public synchronized Builder valueBool(boolean value) {
        FastDbNative.builderValueBool(address(), value);
        return this;
    }

    public synchronized Builder valueU8(int value) {
        FastDbNative.builderValueU8(address(), (int) NativeValues.unsigned(value, 0xff, "u8"));
        return this;
    }

    public synchronized Builder valueU16(int value) {
        FastDbNative.builderValueU16(address(), (int) NativeValues.unsigned(value, 0xffff, "u16"));
        return this;
    }

    public synchronized Builder valueU32(long value) {
        FastDbNative.builderValueU32(address(), NativeValues.unsigned(value, 0xffff_ffffL, "u32"));
        return this;
    }

    public synchronized Builder valueI32(int value) {
        FastDbNative.builderValueI32(address(), value);
        return this;
    }

    public synchronized Builder valueU8n(double value) {
        FastDbNative.builderValueU8n(address(), Double.doubleToRawLongBits(value));
        return this;
    }

    public synchronized Builder valueU16n(double value) {
        FastDbNative.builderValueU16n(address(), Double.doubleToRawLongBits(value));
        return this;
    }

    public synchronized Builder valueF32(float value) {
        FastDbNative.builderValueF32(address(), Float.floatToRawIntBits(value));
        return this;
    }

    public synchronized Builder valueF64(double value) {
        FastDbNative.builderValueF64(address(), Double.doubleToRawLongBits(value));
        return this;
    }

    public synchronized Builder valueComponentBegin() {
        FastDbNative.builderValueComponentBegin(address());
        return this;
    }

    public synchronized Builder valueListBegin(long value) {
        NativeValues.requireNonNegative(value);
        FastDbNative.builderValueListBegin(address(), value);
        return this;
    }

    public synchronized Builder objectFillBegin(ObjectHandle value) {
        FastDbNative.builderObjectFillBegin(address(), value.token());
        return this;
    }

    public synchronized Builder valueObject(ObjectHandle value) {
        FastDbNative.builderValueObject(address(), value.token());
        return this;
    }

    public synchronized Builder valueRef(ObjectHandle value) {
        FastDbNative.builderValueRef(address(), value.token());
        return this;
    }

    public synchronized Builder valueStr(String value) {
        FastDbNative.builderValueStr(address(), NativeValues.utf8(value));
        return this;
    }

    public synchronized Builder valueWstr(String value) {
        FastDbNative.builderValueWstr(address(), Objects.requireNonNull(value));
        return this;
    }

    public Builder valueBytes(byte[] bytes) {
        return valueBytes(bytes, 0, bytes.length);
    }

    public synchronized Builder valueBytes(byte[] bytes, int offset, int length) {
        NativeValues.requireSlice(bytes, offset, length);
        FastDbNative.builderValueBytes(address(), bytes, offset, length);
        return this;
    }

    /**
     * Authors the remaining bytes without advancing the input buffer. For direct
     * buffers, keep the selected bytes unchanged until the builder and any
     * frozen plan have released them.
     */
    public synchronized Builder valueBytes(ByteBuffer bytes) {
        if (bytes.isDirect()) {
            FastDbNative.builderValueDirectBytes(address(), bytes, bytes.position(), bytes.remaining());
        } else if (bytes.hasArray()) {
            return valueBytes(bytes.array(), bytes.arrayOffset() + bytes.position(), bytes.remaining());
        } else {
            return valueBytes(NativeValues.copyRemaining(bytes));
        }
        return this;
    }

    public synchronized BuildPlan freeze() {
        BuildPlan result = new BuildPlan();
        result.adopt(FastDbNative.builderFreeze(address()));
        return result;
    }
}
