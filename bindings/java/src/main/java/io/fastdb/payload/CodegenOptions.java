package io.fastdb.payload;

public record CodegenOptions(int flags, long maxArtifacts,
                             long maxTotalBytes) {
    public CodegenOptions {
        NativeValues.requireNonNegative(maxArtifacts, maxTotalBytes);
    }

    public static CodegenOptions defaults() {
        long[] raw = FastDbNative.codegenOptions();
        return new CodegenOptions((int) raw[0], raw[1], raw[2]);
    }

    long[] toNative() {
        return new long[] {Integer.toUnsignedLong(flags), maxArtifacts, maxTotalBytes};
    }
}
