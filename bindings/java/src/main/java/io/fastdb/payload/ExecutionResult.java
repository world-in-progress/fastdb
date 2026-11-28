package io.fastdb.payload;

/** Owns a payload whose lifetime may also be managed directly through payload(). */
public record ExecutionResult(Payload payload, ExecutionReport report) implements AutoCloseable {
    @Override
    public void close() {
        payload.close();
    }
}
