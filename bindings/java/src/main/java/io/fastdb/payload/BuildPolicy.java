package io.fastdb.payload;

public enum BuildPolicy {
    ALLOW_STAGING(1),
    REQUIRE_DIRECT(2);

    private final int code;

    BuildPolicy(int code) {
        this.code = code;
    }

    public int code() {
        return code;
    }
}
