package io.fastdb.payload;

import java.util.HexFormat;
import java.util.Objects;

/** Independent snapshot of a Core-generated artifact. */
public final class Artifact {
    private final String relativePath;
    private final ArtifactKind kind;
    private final byte[] bytes;
    private final byte[] sha256;

    Artifact(String relativePath, ArtifactKind kind, byte[] bytes, byte[] sha256) {
        this.relativePath = Objects.requireNonNull(relativePath);
        this.kind = Objects.requireNonNull(kind);
        this.bytes = bytes;
        this.sha256 = sha256;
    }

    public String relativePath() { return relativePath; }
    public ArtifactKind kind() { return kind; }
    public byte[] bytes() { return bytes.clone(); }
    public byte[] sha256() { return sha256.clone(); }
    public String sha256Hex() { return HexFormat.of().formatHex(sha256); }
}
