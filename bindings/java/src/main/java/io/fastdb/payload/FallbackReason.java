package io.fastdb.payload;

public enum FallbackReason {
    NONE(0),
    PLAN_REQUIRES_STAGING(1),
    BACKING_DECLINED_DIRECT(2);

    private final int code;

    FallbackReason(int code) {
        this.code = code;
    }

    public int code() {
        return code;
    }

    static FallbackReason fromCode(int code) {
        return NativeValues.enumValue(values(), FallbackReason::code, code);
    }
}
