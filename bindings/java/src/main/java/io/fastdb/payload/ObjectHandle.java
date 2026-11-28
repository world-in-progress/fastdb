package io.fastdb.payload;

/** A builder-local, non-owning token for a declared graph object. */
public final class ObjectHandle {
    private final long token;

    ObjectHandle(long token) {
        this.token = token;
    }

    long token() {
        return token;
    }
}
