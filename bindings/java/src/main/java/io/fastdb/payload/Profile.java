package io.fastdb.payload;

public enum Profile {
    RECORD_V1(1),
    OBJECT_GRAPH_V1(2);

    private final int code;

    Profile(int code) {
        this.code = code;
    }

    public int code() {
        return code;
    }

    static Profile fromCode(int code) {
        return NativeValues.enumValue(values(), Profile::code, code);
    }
}
