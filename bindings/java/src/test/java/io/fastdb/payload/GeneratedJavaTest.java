package io.fastdb.payload;

import static org.junit.jupiter.api.Assertions.*;

import java.io.ByteArrayOutputStream;
import java.lang.reflect.InvocationTargetException;
import java.net.URL;
import java.net.URLClassLoader;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.HexFormat;
import java.util.Locale;
import java.util.StringJoiner;

import javax.tools.ToolProvider;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;

final class GeneratedJavaTest {
    @TempDir
    Path work;

    @Test
    void generatedRecordBuildReadDetachAndInvalidate() throws Exception {
        byte[] source = GoldensTest.read("spec/valid/record-all-types.source.json");
        try (CompiledSpec original = CompiledSpec.compile(source);
             Generated generated = generate(original);
             CompiledSpec spec = generated.compileSpec();
             Builder builder = Builder.create(spec)) {
            assertEquals(original.sha256Hex(), spec.sha256Hex());
            assertSame(builder, generated.begin(builder, "single", 1));
            builder.valueComponentBegin().valueBool(true).valueU8(255).valueU16(65535)
                    .valueU32(0xffff_ffffL).valueI32(-3).valueU8n(1).valueU16n(-1)
                    .valueF32(-0.0f).valueF64(2.5).valueStr("中😀\0").valueWstr("A😀\0")
                    .valueBytes(new byte[] {0, (byte) 0xff}).valueComponentBegin()
                    .valueListBegin(2).valueNull().valueListBegin(2).valueStr("nested").valueNull();
            generated.begin(builder, "series", 2).valueNull().valueListBegin(0);

            try (BuildPlan plan = builder.freeze();
                 ExecutionResult result = plan.execute(BuildPolicy.ALLOW_STAGING);
                 AutoCloseable entry = generated.entry(result.payload(), "single");
                 View row = (View) invoke(entry, "at", new Class<?>[] {long.class}, 0L);
                 AutoCloseable typed = generated.component("AllTypes", row);
                 AutoCloseable detached = (AutoCloseable) invoke(typed, "materialize");
                 View text = field(typed, "str_value");
                 View wide = field(typed, "wstr_value");
                 View opaque = field(typed, "bytes_value");
                 View outerList = field(typed, "list_value");
                 View inner = outerList.at(1);
                 View absent = inner.at(1)) {
                assertEquals(1L, invoke(entry, "length"));
                assertEquals(true, scalar(typed, "boolean_value"));
                assertEquals(255, scalar(typed, "u8_value"));
                assertEquals(65535, scalar(typed, "u16_value"));
                assertEquals(0xffff_ffffL, scalar(typed, "u32_value"));
                assertEquals(-3, scalar(typed, "i32_value"));
                assertEquals(1.0, scalar(typed, "u8n_value"));
                assertEquals(-1.0, scalar(typed, "u16n_value"));
                assertEquals(-0.0f, scalar(typed, "f32_value"));
                assertEquals(2.5, scalar(typed, "f64_value"));
                assertEquals("中😀\0", text.getStr());
                assertEquals("A😀\0", wide.getWstr());
                assertArrayEquals(new byte[] {0, (byte) 0xff}, opaque.getBytes());
                assertTrue(absent.isNull());

                result.payload().invalidate();
                result.payload().invalidate();
                result.payload().close();
                assertInvalidated(() -> invoke(entry, "length"));
                assertInvalidated(() -> scalar(typed, "u8_value"));
                assertInvalidated(text::getStr);
                assertInvalidated(wide::getWstr);
                assertInvalidated(opaque::getBytes);
                assertInvalidated(absent::isNull);
                assertEquals(255, scalar(detached, "u8_value"));
            }
        }
    }

    @Test
    void generatedGraphPreservesCyclesSharingAndDetachedReads() throws Exception {
        try (CompiledSpec original = GoldensTest.spec("graph-all-values");
             Generated generated = generate(original);
             CompiledSpec spec = generated.compileSpec();
             Builder builder = Builder.create(spec)) {
            ObjectHandle node = generated.declare(builder, "Node");
            ObjectHandle asset = generated.declare(builder, "Asset");
            GoldenScenarios.graphAllValues(builder, node, asset);
            try (BuildPlan plan = builder.freeze();
                 ExecutionResult result = plan.execute(BuildPolicy.ALLOW_STAGING);
                 AutoCloseable root = generated.entry(result.payload(), "root");
                 AutoCloseable assets = generated.entry(result.payload(), "asset");
                 AutoCloseable refs = generated.entry(result.payload(), "refs");
                 View row = (View) invoke(root, "at", new Class<?>[] {long.class}, 0L);
                 AutoCloseable typed = generated.component("Node", row);
                 View self = (View) invoke(typed, symbol("o_peer") + "_refTarget");
                 View shared = (View) invoke(typed, symbol("p_asset") + "_refTarget");
                 AutoCloseable assetView = generated.component("Asset", shared);
                 View cycleBack = (View) invoke(assetView, symbol("owner") + "_refTarget");
                 View ref = (View) invoke(refs, "atRefTarget", new Class<?>[] {long.class}, 0L);
                 AutoCloseable detached = (AutoCloseable) invoke(typed, "materialize");
                 View detachedPeer = (View) invoke(detached, symbol("o_peer") + "_refTarget")) {
                GraphIdentity identity = row.graphIdentity();
                assertEquals(identity, invoke(root, "atGraphIdentity", new Class<?>[] {long.class}, 0L));
                assertEquals(identity, invoke(typed, "graphIdentity"));
                assertEquals(identity, self.graphIdentity());
                assertEquals(identity, cycleBack.graphIdentity());
                assertEquals(identity, ref.graphIdentity());
                assertEquals(shared.graphIdentity(), invoke(assets, "atGraphIdentity", new Class<?>[] {long.class}, 0L));
                assertEquals(0x789abcdeL, scalar(typed, "d_u32"));
                try (View text = field(typed, "j_str");
                     View wide = field(typed, "k_wstr");
                     View opaque = field(typed, "l_bytes");
                     View list = field(typed, "n_values");
                     View absent = list.at(1)) {
                    assertEquals("same", text.getStr());
                    assertEquals("A😀", wide.getWstr());
                    assertArrayEquals(new byte[] {0, (byte) 0xff, 0x7e}, opaque.getBytes());
                    assertTrue(absent.isNull());
                }
                assertArrayEquals(GoldensTest.binary("graph-all-values"), result.payload().binaryBytes());
                result.payload().invalidate();
                result.payload().close();
                assertInvalidated(ref::graphIdentity);
                assertInvalidated(() -> scalar(typed, "d_u32"));
                assertEquals(0x789abcdeL, scalar(detached, "d_u32"));
                try (View detachedNumber = detachedPeer.field(spec.componentFieldIndex(spec.componentIndex("Node"), "d_u32"))) {
                    assertEquals(0x789abcdeL, detachedNumber.getU32());
                }
            }
        }
    }

    @Test
    void generatedLargeSpecCompilesAndRuns() throws Exception {
        var entries = new StringJoiner(",");
        for (int i = 0; i < 280; i++) {
            entries.add(String.format(Locale.ROOT, """
                    {"id":"entry_%03d","cardinality":"many","type":{"kind":"u8"}}
                    """, i));
        }
        String source = """
                {"schema":"fastdb.payload.v1","profile":"record.v1","entries":[%s],"components":[]}
                """.formatted(entries);
        try (CompiledSpec original = CompiledSpec.compile(source);
             Generated generated = generate(original);
             CompiledSpec spec = generated.compileSpec();
             Builder builder = Builder.create(spec)) {
            assertTrue(original.canonicalJson().length > 16 * 1024);
            assertArrayEquals(original.canonicalJson(), (byte[]) invoke(generated.type(), "canonicalSource", new Class<?>[0]));
            assertArrayEquals(original.sha256(), spec.sha256());
            generated.begin(builder, "entry_000", 1).valueU8(42);
            for (int i = 1; i < spec.entryCount(); i++) {
                builder.entryBegin(i, 0);
            }
            try (BuildPlan plan = builder.freeze();
                 ExecutionResult result = plan.execute(BuildPolicy.ALLOW_STAGING);
                 AutoCloseable entry = generated.entry(result.payload(), "entry_000");
                 View first = (View) invoke(entry, "at", new Class<?>[] {long.class}, 0L)) {
                assertEquals(42, first.getU8());
            }
        }
    }

    private Generated generate(CompiledSpec spec) throws Exception {
        Artifact artifact = spec.generate(CodegenTarget.JAVA).artifact(0);
        Path source = work.resolve(artifact.relativePath());
        Files.createDirectories(source.getParent());
        Files.write(source, artifact.bytes());
        Path classes = Files.createDirectories(work.resolve("classes"));
        Path api = Path.of(CompiledSpec.class.getProtectionDomain().getCodeSource().getLocation().toURI());
        var compiler = ToolProvider.getSystemJavaCompiler();
        assertNotNull(compiler, "Tests require a JDK");
        var output = new ByteArrayOutputStream();
        int exit = compiler.run(null, output, output, "--release", "17", "-encoding", "UTF-8",
                "-Xlint:all", "-Werror", "-classpath", api.toString(), "-d", classes.toString(), source.toString());
        assertEquals(0, exit, () -> output.toString(StandardCharsets.UTF_8));
        var loader = new URLClassLoader(new URL[] {classes.toUri().toURL()}, CompiledSpec.class.getClassLoader());
        try {
            String typeName = artifact.relativePath().replace('/', '.').replaceFirst("\\.java$", "");
            return new Generated(loader, loader.loadClass(typeName));
        } catch (Exception | Error failure) {
            loader.close();
            throw failure;
        }
    }

    private static String symbol(String id) {
        return "fdb_java_id_" + HexFormat.of().formatHex(id.getBytes(StandardCharsets.UTF_8));
    }

    private static Object invoke(Object target, String method) throws ReflectiveOperationException {
        return invoke(target, method, new Class<?>[0]);
    }

    private static Object invoke(Object target, String name, Class<?>[] parameters, Object... args)
            throws ReflectiveOperationException {
        Class<?> type = target instanceof Class<?> cls ? cls : target.getClass();
        try {
            return type.getMethod(name, parameters).invoke(target instanceof Class<?> ? null : target, args);
        } catch (InvocationTargetException failure) {
            if (failure.getCause() instanceof RuntimeException runtime) {
                throw runtime;
            }
            if (failure.getCause() instanceof Error error) {
                throw error;
            }
            throw failure;
        }
    }

    private static View field(AutoCloseable typed, String id) throws ReflectiveOperationException {
        return (View) invoke(typed, symbol(id));
    }

    private static Object scalar(AutoCloseable typed, String id) throws ReflectiveOperationException {
        return invoke(typed, symbol(id) + "_value");
    }

    private static void assertInvalidated(org.junit.jupiter.api.function.Executable action) {
        FastDbException error = assertThrows(FastDbException.class, action);
        assertEquals(FastDbException.E_VIEW_INVALIDATED, error.code());
        assertEquals("VIEW_INVALIDATED", error.symbol());
    }

    private record Generated(URLClassLoader loader, Class<?> type) implements AutoCloseable {
        CompiledSpec compileSpec() throws ReflectiveOperationException {
            return (CompiledSpec) invoke(type, "compileSpec", new Class<?>[0]);
        }

        Builder begin(Builder builder, String id, long count) throws ReflectiveOperationException {
            return (Builder) invoke(type, symbol(id) + "_builderEntryBegin",
                    new Class<?>[] {Builder.class, long.class}, builder, count);
        }

        ObjectHandle declare(Builder builder, String component) throws ReflectiveOperationException {
            return (ObjectHandle) invoke(type, symbol(component) + "_builderDeclare", new Class<?>[] {Builder.class}, builder);
        }

        AutoCloseable entry(Payload payload, String id) throws ReflectiveOperationException {
            return (AutoCloseable) invoke(type, symbol(id) + "_fromPayload", new Class<?>[] {Payload.class}, payload);
        }

        AutoCloseable component(String id, View view) throws ReflectiveOperationException {
            Class<?> typed = loader.loadClass(type.getName() + "$FdbJavaType_" + symbol(id) + "_View");
            return (AutoCloseable) invoke(typed, "tryFromView", new Class<?>[] {View.class}, view);
        }

        @Override
        public void close() throws java.io.IOException {
            loader.close();
        }
    }
}
