package io.fastdb.payload;

import java.nio.ByteBuffer;
import java.nio.CharBuffer;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.StandardCharsets;
import java.util.Objects;
import java.util.function.ToIntFunction;

final class NativeValues {
    private NativeValues() {}

    static String text(byte[] utf8) {
        return new String(utf8, StandardCharsets.UTF_8);
    }

    static byte[] utf8(String text) {
        try {
            ByteBuffer encoded = StandardCharsets.UTF_8.newEncoder().encode(CharBuffer.wrap(text));
            byte[] bytes = new byte[encoded.remaining()];
            encoded.get(bytes);
            return bytes;
        } catch (CharacterCodingException malformed) {
            // Pass an invalid UTF-8 byte to Core so it reports the failure at the
            // original operation's path. The default Java encoder replaces surrogates.
            return new byte[] {(byte) 0xff};
        }
    }

    static byte[] digest(byte[] bytes) {
        if (bytes.length != 32) {
            throw new IllegalArgumentException("Expected a 32-byte SHA-256 digest");
        }
        return bytes;
    }

    static byte[] copyRemaining(ByteBuffer buffer) {
        byte[] bytes = new byte[buffer.remaining()];
        buffer.duplicate().get(bytes);
        return bytes;
    }

    static long unsigned(long value, long max, String type) {
        if (value < 0 || value > max) {
            throw new FastDbException(FastDbException.E_OUT_OF_RANGE, "OUT_OF_RANGE", "",
                    type + " must be in 0.." + max, "{}");
        }
        return value;
    }

    static void requireNonNegative(long... values) {
        for (long value : values) {
            if (value < 0) {
                throw new IllegalArgumentException("Limits must be nonnegative");
            }
        }
    }

    static <E> E enumValue(E[] values, ToIntFunction<E> codeOf, int code) {
        for (E value : values) {
            if (codeOf.applyAsInt(value) == code) {
                return value;
            }
        }
        throw new IllegalStateException("Unknown FastDB ABI value " + code);
    }

    static void requireSlice(byte[] bytes, int offset, int length) {
        Objects.checkFromIndexSize(offset, length, bytes.length);
    }
}
