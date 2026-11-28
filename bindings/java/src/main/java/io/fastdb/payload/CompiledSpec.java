package io.fastdb.payload;

import java.util.ArrayList;
import java.util.HexFormat;

/** Immutable compiled portable schema owned by Core. */
public final class CompiledSpec extends NativeHandle {
    private CompiledSpec() {
        super(FastDbNative::specRelease);
    }

    public static CompiledSpec compile(byte[] source) {
        return compile(source, null);
    }

    public static CompiledSpec compile(String source) {
        return compile(NativeValues.utf8(source));
    }

    public static CompiledSpec compile(byte[] source, CompileOptions options) {
        CompiledSpec result = new CompiledSpec();
        result.adopt(FastDbNative.specCompile(source, source.length, options == null ? null : options.toNative()));
        return result;
    }

    public synchronized CompiledSpec retain() {
        CompiledSpec result = new CompiledSpec();
        result.adopt(FastDbNative.specRetain(address()));
        return result;
    }

    public synchronized byte[] sha256() {
        return FastDbNative.specSha256(address());
    }

    public String sha256Hex() {
        return HexFormat.of().formatHex(sha256());
    }

    public synchronized byte[] canonicalJson() {
        return FastDbNative.specCanonicalJson(address());
    }

    public synchronized byte[] manifestJson() {
        return FastDbNative.specManifestJson(address());
    }

    public synchronized Profile profile() {
        return Profile.fromCode(FastDbNative.specProfile(address()));
    }

    public synchronized Capabilities capabilities() {
        return Capabilities.fromNative(FastDbNative.specCapabilities(address()));
    }

    public ArtifactSet generate(CodegenTarget target) {
        return generate(target, null);
    }

    public synchronized ArtifactSet generate(CodegenTarget target, CodegenOptions options) {
        try (CodegenResult result = new CodegenResult()) {
            result.adopt(FastDbNative.specGenerate(address(), target.code(),
                    options == null ? null : options.toNative()));
            long handle = result.address();
            int count = Math.toIntExact(FastDbNative.artifactCount(handle));
            var artifacts = new ArrayList<Artifact>(count);
            for (int index = 0; index < count; index++) {
                String path = NativeValues.text(FastDbNative.artifactPath(handle, index));
                ArtifactKind kind = ArtifactKind.fromCode(FastDbNative.artifactKind(handle, index));
                artifacts.add(new Artifact(path, kind, FastDbNative.artifactBytes(handle, index),
                        FastDbNative.artifactSha256(handle, index)));
            }
            return new ArtifactSet(artifacts);
        }
    }

    public synchronized int entryCount() {
        return FastDbNative.specEntryCount(address());
    }

    public synchronized String entryId(int index) {
        return NativeValues.text(FastDbNative.specEntryId(address(), index));
    }

    public synchronized int entryIndex(String id) {
        return FastDbNative.specEntryIndex(address(), NativeValues.utf8(id));
    }

    public synchronized int componentCount() {
        return FastDbNative.specComponentCount(address());
    }

    public synchronized String componentId(int index) {
        return NativeValues.text(FastDbNative.specComponentId(address(), index));
    }

    public synchronized int componentIndex(String id) {
        return FastDbNative.specComponentIndex(address(), NativeValues.utf8(id));
    }

    public synchronized int componentFieldCount(int componentIndex) {
        return FastDbNative.specComponentFieldCount(address(), componentIndex);
    }

    public synchronized String componentFieldId(int componentIndex, int index) {
        return NativeValues.text(FastDbNative.specComponentFieldId(address(), componentIndex, index));
    }

    public synchronized int componentFieldIndex(int componentIndex, String id) {
        return FastDbNative.specComponentFieldIndex(address(), componentIndex, NativeValues.utf8(id));
    }

    public static byte[] sourceSchemaJson() {
        return FastDbNative.sourceSchemaJson();
    }

    public static byte[] sourceSchemaSha256() {
        return FastDbNative.sourceSchemaSha256();
    }

    private static final class CodegenResult extends NativeHandle {
        CodegenResult() {
            super(FastDbNative::artifactResultRelease);
        }
    }
}
