package io.fastdb.payload;

/**
 * Borrowed addresses conforming to the stable C ABI. Callers keep the data
 * readable and unchanged, and the nonthrowing C callbacks callable until Core
 * releases its retained reference. Addresses must be valid in this process.
 */
public record NativeBytesBacking(long dataPtr, long byteLen, long retainFnPtr,
                                 long releaseFnPtr, long ownerToken) {
    public NativeBytesBacking {
        NativeValues.requireNonNegative(byteLen);
        if ((byteLen != 0 && dataPtr == 0) || retainFnPtr == 0 || releaseFnPtr == 0) {
            throw new IllegalArgumentException("Expected data and both C backing callbacks");
        }
    }
}
