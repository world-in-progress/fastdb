package io.fastdb.payload;

/** Object coordinates scoped to the source payload. */
public record GraphIdentity(int componentIndex, long objectId) {}
