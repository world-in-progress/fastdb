package io.fastdb.payload;

public record BuilderOptions(int flags, long maxValueNodes,
                             long maxListElements, long maxTextBytes,
                             long maxOpaqueBytes, long maxNestingDepth,
                             long maxTotalBuilderBytes, long maxGraphObjects) {
    public BuilderOptions {
        NativeValues.requireNonNegative(maxValueNodes, maxListElements, maxTextBytes, maxOpaqueBytes, maxNestingDepth, maxTotalBuilderBytes, maxGraphObjects);
    }

    public static BuilderOptions defaults() {
        long[] raw = FastDbNative.builderOptions();
        return new BuilderOptions((int) raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], raw[7]);
    }

    long[] toNative() {
        return new long[] {Integer.toUnsignedLong(flags), maxValueNodes, maxListElements, maxTextBytes, maxOpaqueBytes, maxNestingDepth, maxTotalBuilderBytes, maxGraphObjects};
    }
}
