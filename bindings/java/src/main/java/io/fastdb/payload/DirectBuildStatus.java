package io.fastdb.payload;

public enum DirectBuildStatus {
    NOT_EVALUATED(0),
    ELIGIBLE(1),
    UNAVAILABLE(2);

    private final int code;

    DirectBuildStatus(int code) {
        this.code = code;
    }

    public int code() {
        return code;
    }

    static DirectBuildStatus fromCode(int code) {
        return NativeValues.enumValue(values(), DirectBuildStatus::code, code);
    }
}
