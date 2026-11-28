package io.fastdb.payload;

import java.util.List;

/** Java-owned snapshot. The native codegen result is released before returning. */
public record ArtifactSet(List<Artifact> artifacts) {
    public ArtifactSet {
        artifacts = List.copyOf(artifacts);
    }

    public int size() { return artifacts.size(); }
    public Artifact artifact(int index) { return artifacts.get(index); }
}
