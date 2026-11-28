package io.fastdb.payload;

import static org.junit.jupiter.api.Assertions.*;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.HexFormat;
import java.util.List;
import java.util.Locale;
import java.util.regex.Pattern;

import org.junit.jupiter.api.DynamicTest;
import org.junit.jupiter.api.TestFactory;

final class GoldensTest {
    private static final HexFormat HEX = HexFormat.of();
    private static final Path ROOT = Path.of(System.getProperty("fastdb.golden.path"));

    static byte[] read(String name) throws IOException {
        return Files.readAllBytes(ROOT.resolve(name));
    }

    static CompiledSpec spec(String name) throws IOException {
        return CompiledSpec.compile(read("binary/spec/" + name + ".source.json"));
    }

    static byte[] binary(String name) throws IOException {
        return hexFile("binary/valid/" + name + ".bin.hex");
    }

    private static String textFile(String name) throws IOException {
        return Files.readString(ROOT.resolve(name));
    }

    private static byte[] hexFile(String name) throws IOException {
        return HEX.parseHex(textFile(name).replaceAll("\\s+", ""));
    }

    // The golden indexes identify cases with a unique label and use scalar
    // strings for all file references. This reader is specific to these fixtures.
    private static List<String> cases(String filename, String label) throws IOException {
        String index = textFile(filename);
        var starts = new ArrayList<Integer>();
        var marker = Pattern.compile("\"" + label + "\"\\s*:").matcher(index);
        while (marker.find()) {
            starts.add(marker.start());
        }
        assertFalse(starts.isEmpty(), filename);
        starts.add(index.length());
        var entries = new ArrayList<String>();
        for (int i = 0; i + 1 < starts.size(); i++) {
            entries.add(index.substring(starts.get(i), starts.get(i + 1)));
        }
        return entries;
    }

    private static String string(String item, String key) {
        var field = Pattern.compile("\"" + key + "\"\\s*:\\s*\"((?:[^\"\\\\]|\\\\.)*)\"").matcher(item);
        assertTrue(field.find(), () -> "Expected golden field " + key + " in " + item);
        return field.group(1).replace("\\\"", "\"");
    }

    private static long number(String item, String key) {
        var field = Pattern.compile("\"" + key + "\"\\s*:\\s*([0-9]+)").matcher(item);
        assertTrue(field.find(), () -> "Expected golden field " + key + " in " + item);
        return Long.parseLong(field.group(1));
    }

    private static void checkError(String expected, FastDbException error) {
        assertEquals(number(expected, "status"), error.code());
        assertEquals(string(expected, "symbol"), error.symbol());
        assertEquals(string(expected, "path"), error.path());
        assertEquals(string(expected, "message"), error.message());
        assertEquals(string(expected, "details_json"), error.detailsJson());
    }

    @TestFactory
    List<DynamicTest> specGoldens() throws IOException {
        var tests = new ArrayList<DynamicTest>();
        for (String item : cases("spec/index.json", "name")) {
            tests.add(DynamicTest.dynamicTest(string(item, "name"), () -> {
                byte[] source = read("spec/" + string(item, "source"));
                if (item.contains("\"expectation\"")) {
                    var error = assertThrows(FastDbException.class, () -> CompiledSpec.compile(source).close());
                    checkError(textFile("spec/" + string(item, "expectation")), error);
                    return;
                }
                try (CompiledSpec spec = CompiledSpec.compile(source, CompileOptions.defaults());
                     CompiledSpec retained = spec.retain()) {
                    assertArrayEquals(hexFile("spec/" + string(item, "canonical_hex")), retained.canonicalJson());
                    assertArrayEquals(hexFile("spec/" + string(item, "manifest_hex")), spec.manifestJson());
                    assertEquals(textFile("spec/" + string(item, "sha256")).strip(), spec.sha256Hex());
                    Capabilities caps = spec.capabilities();
                    assertEquals(spec.profile(), caps.profile());
                    assertNotEquals(0L, caps.codegenTargetFlags() & CodegenTarget.JAVA.code());
                    for (int i = 0; i < spec.entryCount(); i++) {
                        assertEquals(i, spec.entryIndex(spec.entryId(i)));
                    }
                    for (int i = 0; i < spec.componentCount(); i++) {
                        assertEquals(i, spec.componentIndex(spec.componentId(i)));
                        for (int j = 0; j < spec.componentFieldCount(i); j++) {
                            assertEquals(j, spec.componentFieldIndex(i, spec.componentFieldId(i, j)));
                        }
                    }
                }
            }));
        }
        return tests;
    }

    @TestFactory
    List<DynamicTest> binaryGoldens() throws IOException {
        var tests = new ArrayList<DynamicTest>();
        for (String item : cases("binary/index.json", "name")) {
            tests.add(DynamicTest.dynamicTest(string(item, "name"), () -> {
                byte[] image = hexFile("binary/" + string(item, "binary_hex"));
                try (CompiledSpec spec = CompiledSpec.compile(read("binary/" + string(item, "source")))) {
                    if (item.contains("\"expectation\"")) {
                        OpenOptions options = OpenOptions.defaults();
                        if (item.contains("\"max_total_bytes\"")) {
                            options = options.withMaxTotalBytes(number(item, "max_total_bytes"));
                        }
                        OpenOptions limits = options;
                        var error = assertThrows(FastDbException.class, () -> Payload.openCopy(spec, image, limits).close());
                        checkError(textFile("binary/" + string(item, "expectation")), error);
                        return;
                    }
                    try (Payload opened = Payload.openCopy(spec, image);
                         Builder builder = Builder.create(spec, BuilderOptions.defaults())) {
                        assertArrayEquals(image, opened.binaryBytes());
                        assertArrayEquals(spec.sha256(), opened.sha256());
                        assertEquals(spec.profile(), opened.profile());
                        GoldenScenarios.author(spec, builder, string(item, "scenario"));
                        try (BuildPlan plan = builder.freeze();
                             ExecutionResult built = plan.execute(BuildPolicy.ALLOW_STAGING)) {
                            byte[] actual = built.payload().binaryBytes();
                            assertArrayEquals(image, actual);
                            assertEquals(textFile("binary/" + string(item, "sha256")).strip(),
                                    HEX.formatHex(MessageDigest.getInstance("SHA-256").digest(actual)));
                            assertEquals(actual.length, plan.info().totalBytes());
                            assertEquals(built.report(), built.payload().executionReport());
                        }
                    }
                }
            }));
        }
        return tests;
    }

    @TestFactory
    List<DynamicTest> codegenGoldens() throws IOException {
        var tests = new ArrayList<DynamicTest>();
        String index = textFile("codegen/index.json");
        for (String item : cases("codegen/index.json", "target")) {
            String target = string(item, "target");
            tests.add(DynamicTest.dynamicTest(target, () -> {
                try (CompiledSpec spec = CompiledSpec.compile(read("spec/valid/empty-record.source.json"))) {
                    assertEquals(string(index, "payload_sha256"), spec.sha256Hex());
                    ArtifactSet artifacts = spec.generate(CodegenTarget.valueOf(target.toUpperCase(Locale.ROOT)),
                            CodegenOptions.defaults());
                    assertEquals(1, artifacts.size());
                    Artifact artifact = artifacts.artifact(0);
                    assertEquals(string(item, "relative_path"), artifact.relativePath());
                    assertEquals(string(item, "kind"), artifact.kind().name().toLowerCase(Locale.ROOT));
                    assertEquals(string(item, "sha256"), artifact.sha256Hex());
                    assertArrayEquals(read("codegen/" + string(item, "golden")), artifact.bytes());
                }
            }));
        }
        return tests;
    }
}
