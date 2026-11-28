package io.fastdb.payload;

public enum ExecutionMode {
    DIRECT(1),
    STAGED(2);

    private final int code;

    ExecutionMode(int code) {
        this.code = code;
    }

    public int code() {
        return code;
    }

    static ExecutionMode fromCode(int code) {
        return NativeValues.enumValue(values(), ExecutionMode::code, code);
    }
}
