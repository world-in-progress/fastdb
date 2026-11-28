package io.fastdb.payload;

public record OpenOptions(int flags, long maxTotalBytes,
                          long maxRegions, long maxEntries,
                          long maxComponents, long maxNestingDepth,
                          long maxListElements, long maxGraphObjects,
                          long maxStringBytes, long maxValidationWork) {
    public OpenOptions {
        NativeValues.requireNonNegative(maxTotalBytes, maxRegions, maxEntries, maxComponents, maxNestingDepth, maxListElements, maxGraphObjects, maxStringBytes, maxValidationWork);
    }

    public static final int VALIDATE_TEXT_EAGER = 1;

    public OpenOptions withMaxTotalBytes(long limit) {
        return new OpenOptions(flags, limit, maxRegions, maxEntries, maxComponents,
                maxNestingDepth, maxListElements, maxGraphObjects, maxStringBytes, maxValidationWork);
    }

    public OpenOptions withEagerTextValidation(boolean enabled) {
        int newFlags = enabled ? flags | VALIDATE_TEXT_EAGER : flags & ~VALIDATE_TEXT_EAGER;
        return new OpenOptions(newFlags, maxTotalBytes, maxRegions, maxEntries, maxComponents,
                maxNestingDepth, maxListElements, maxGraphObjects, maxStringBytes, maxValidationWork);
    }

    public static OpenOptions defaults() {
        long[] raw = FastDbNative.openOptions();
        return new OpenOptions((int) raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], raw[7], raw[8], raw[9]);
    }

    long[] toNative() {
        return new long[] {Integer.toUnsignedLong(flags), maxTotalBytes, maxRegions, maxEntries, maxComponents, maxNestingDepth, maxListElements, maxGraphObjects, maxStringBytes, maxValidationWork};
    }
}
