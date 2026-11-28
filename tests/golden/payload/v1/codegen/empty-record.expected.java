// generated-by: fastdb.payload.codegen.v1
// payload-sha256: 92fbbc65fc79ad9ca6e9637063c8f15a806b40e788532225cafe25ef17cdfc71
// core-abi-version: 1
// generator-version: fastdb.payload.codegen.v1
// target: java

package io.fastdb.payload.generated;

import io.fastdb.payload.CompiledSpec;
import java.nio.charset.StandardCharsets;
import java.util.List;

public final class FastdbPayload_92fbbc65fc79ad9ca6e9637063c8f15a806b40e788532225cafe25ef17cdfc71 {
    private FastdbPayload_92fbbc65fc79ad9ca6e9637063c8f15a806b40e788532225cafe25ef17cdfc71() {}

    public static final String PAYLOAD_SHA256 = "92fbbc65fc79ad9ca6e9637063c8f15a806b40e788532225cafe25ef17cdfc71";

    public static byte[] payloadSha256() {
        return new byte[] {
            (byte) 0x92, (byte) 0xfb, (byte) 0xbc, (byte) 0x65, (byte) 0xfc, (byte) 0x79, (byte) 0xad, (byte) 0x9c,
            (byte) 0xa6, (byte) 0xe9, (byte) 0x63, (byte) 0x70, (byte) 0x63, (byte) 0xc8, (byte) 0xf1, (byte) 0x5a,
            (byte) 0x80, (byte) 0x6b, (byte) 0x40, (byte) 0xe7, (byte) 0x88, (byte) 0x53, (byte) 0x22, (byte) 0x25,
            (byte) 0xca, (byte) 0xfe, (byte) 0x25, (byte) 0xef, (byte) 0x17, (byte) 0xcd, (byte) 0xfc, (byte) 0x71,
        };
    }

    public static String canonicalSourceText() {
        return "{\"components\":[],\"entries\":[],\"profile\":\"record.v1\",\"schema\":\"fastdb.payload.v1\"}";
    }

    public static byte[] canonicalSource() {
        return canonicalSourceText().getBytes(StandardCharsets.UTF_8);
    }

    public static CompiledSpec compileSpec() {
        return CompiledSpec.compile(canonicalSource());
    }

    public record IdMetadata(String symbol, String originalId, int stableIndex) {}
    public record FieldMetadata(String symbol, String originalId, int componentIndex, int fieldIndex) {}

    public static final List<IdMetadata> ENTRIES = List.of();
    public static final List<IdMetadata> COMPONENTS = List.of();
    public static final List<FieldMetadata> FIELDS = List.of();
}
