package io.fastdb.payload;

public enum ViewKind {
    SEQUENCE(1),
    BOOL(2),
    U8(3),
    U16(4),
    U32(5),
    I32(6),
    U8N(7),
    U16N(8),
    F32(9),
    F64(10),
    STR(11),
    WSTR(12),
    BYTES(13),
    COMPONENT(14),
    LIST(15),
    REF(16);

    private final int code;

    ViewKind(int code) {
        this.code = code;
    }

    public int code() {
        return code;
    }

    static ViewKind fromCode(int code) {
        return NativeValues.enumValue(values(), ViewKind::code, code);
    }
}
