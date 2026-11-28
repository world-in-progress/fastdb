package io.fastdb.payload;

public enum CodegenTarget {
    CPP(1),
    RUST(2),
    PYTHON(4),
    TYPESCRIPT(8),
    JAVA(16);

    private final int code;

    CodegenTarget(int code) {
        this.code = code;
    }

    public int code() {
        return code;
    }
}
