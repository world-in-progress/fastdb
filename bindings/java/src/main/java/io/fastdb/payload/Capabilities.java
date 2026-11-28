package io.fastdb.payload;

public record Capabilities(Profile profile, long semanticFlags, long operationFlags,
                           long codegenTargetFlags, DirectBuildStatus directBuildStatus) {
    public static final long HAS_NULLABLE = 1L;
    public static final long HAS_LISTS = 1L << 1;
    public static final long HAS_REFERENCES = 1L << 2;
    public static final long HAS_VARIABLE_WIDTH = 1L << 3;
    public static final long HAS_NORMALIZED_INTEGERS = 1L << 4;

    public static final long COMPILE = 1L;
    public static final long QUERY = 1L << 1;
    public static final long BUILD = 1L << 2;
    public static final long OPEN = 1L << 3;
    public static final long VIEW = 1L << 4;
    public static final long MATERIALIZE = 1L << 5;
    public static final long INVALIDATE = 1L << 6;
    public static final long CODEGEN = 1L << 7;

    static Capabilities fromNative(long[] raw) {
        return new Capabilities(Profile.fromCode((int) raw[0]), raw[1], raw[2], raw[3],
                DirectBuildStatus.fromCode((int) raw[4]));
    }
}
