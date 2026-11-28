package io.fastdb.payload;

public record PlanInfo(int flags, long totalBytes, long regionCount, long logicalValueCount,
                       long listElementCount, long textBytes, long opaqueBytes, long validationWork,
                       int maxAlignment, DirectBuildStatus directBuildStatus, long graphObjectCount) {
    static PlanInfo fromNative(long[] raw) {
        return new PlanInfo((int) raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], raw[7],
                (int) raw[8], DirectBuildStatus.fromCode((int) raw[9]), raw[10]);
    }
}
