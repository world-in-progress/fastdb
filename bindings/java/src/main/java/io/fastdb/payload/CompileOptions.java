package io.fastdb.payload;

public record CompileOptions(int flags, long maxSourceBytes,
                             long maxJsonValues, int maxNestingDepth,
                             int maxEntries, int maxComponents,
                             int maxFieldsPerComponent, long maxTotalFields) {
    public CompileOptions {
        NativeValues.requireNonNegative(maxSourceBytes, maxJsonValues, maxNestingDepth, maxEntries, maxComponents, maxFieldsPerComponent, maxTotalFields);
    }

    public static CompileOptions defaults() {
        long[] raw = FastDbNative.compileOptions();
        return new CompileOptions((int) raw[0], raw[1], raw[2], (int) raw[3], (int) raw[4], (int) raw[5], (int) raw[6], raw[7]);
    }

    long[] toNative() {
        return new long[] {Integer.toUnsignedLong(flags), maxSourceBytes, maxJsonValues, Integer.toUnsignedLong(maxNestingDepth), Integer.toUnsignedLong(maxEntries), Integer.toUnsignedLong(maxComponents), Integer.toUnsignedLong(maxFieldsPerComponent), maxTotalFields};
    }
}
