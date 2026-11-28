package io.fastdb.payload;

public enum ArtifactKind {
    SOURCE(1);

    private final int code;

    ArtifactKind(int code) {
        this.code = code;
    }

    public int code() {
        return code;
    }

    static ArtifactKind fromCode(int code) {
        return NativeValues.enumValue(values(), ArtifactKind::code, code);
    }
}
