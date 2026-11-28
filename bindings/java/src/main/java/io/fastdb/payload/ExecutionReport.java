package io.fastdb.payload;

public record ExecutionReport(ExecutionMode mode, FallbackReason fallbackReason,
                              long requestedBytes, long usedBytes, long stagingBytes,
                              long regionCount, long backingCapacity) {
    static ExecutionReport fromNative(long[] raw) {
        return new ExecutionReport(ExecutionMode.fromCode((int) raw[0]), FallbackReason.fromCode((int) raw[1]),
                raw[2], raw[3], raw[4], raw[5], raw[6]);
    }
}
