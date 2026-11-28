package io.fastdb.payload;

import static org.junit.jupiter.api.Assertions.*;

import java.nio.ByteBuffer;
import java.nio.file.Path;

import org.junit.jupiter.api.Test;

final class NativeBackingTest {
    static {
        // Load the runtime DLL before loading the test DLL that depends on it.
        CompiledSpec.sourceSchemaSha256();
        String path = System.getProperty("fastdb.java.library.path");
        if (path == null || path.isBlank()) {
            path = System.getenv("FASTDB_JAVA_LIBRARY_PATH");
        }
        if (path == null || path.isBlank()) {
            System.loadLibrary("fastdb_java_test");
        } else {
            System.load(Path.of(path).toAbsolutePath()
                    .resolveSibling(System.mapLibraryName("fastdb_java_test")).toString());
        }
    }

    @Test
    void nativeReadBackingRetainsOnceAndReleasesOnInvalidation() throws Exception {
        byte[] image = GoldensTest.binary("text-bytes");
        try (CompiledSpec spec = GoldensTest.spec("text-bytes");
             Backing backing = new Backing(image);
             Payload payload = Payload.openExternalNative(spec, backing.bytes());
             View entry = payload.entryView(0);
             View text = entry.at(3)) {
            assertArrayEquals(new long[] {1, 0, 0, 0, 0}, counts(backing.address()));
            assertEquals("中\0", text.getStr());
            try (Access access = payload.acquire()) {
                assertEquals(dataAddress(backing.address()), access.dataAddress());
                assertArrayEquals(image, access.toByteArray());
            }
            payload.invalidate();
            payload.invalidate();
            assertArrayEquals(new long[] {1, 1, 0, 0, 0}, counts(backing.address()));
            assertEquals(FastDbException.E_VIEW_INVALIDATED, assertThrows(FastDbException.class, text::getStr).code());
        }
    }

    @Test
    void failedNativeOpenBalancesTheCoreRetain() throws Exception {
        byte[] invalid = GoldensTest.binary("empty");
        invalid[0] ^= 1;
        try (CompiledSpec spec = GoldensTest.spec("empty");
             Backing backing = new Backing(invalid)) {
            assertEquals(FastDbException.E_INVALID_MAGIC,
                    assertThrows(FastDbException.class, () -> Payload.openExternalNative(
                            spec, backing.bytes(), OpenOptions.defaults()).close()).code());
            assertArrayEquals(new long[] {1, 1, 0, 0, 0}, counts(backing.address()));
        }
    }

    @Test
    void directBufferBackingCanReleaseFromAnUnattachedThread() throws Exception {
        byte[] image = GoldensTest.binary("text-bytes");
        ByteBuffer buffer = ByteBuffer.allocateDirect(image.length).put(image).flip();
        try (CompiledSpec spec = GoldensTest.spec("text-bytes");
             Payload payload = Payload.openExternal(spec, buffer);
             View entry = payload.entryView(0)) {
            synchronized (payload) {
                // JNI_EDETACHED before and after the Core call.
                assertArrayEquals(new long[] {-2, 0, -2}, invalidateDetached(payload.address()));
            }
            assertEquals(FastDbException.E_VIEW_INVALIDATED, assertThrows(FastDbException.class, entry::length).code());
        }
    }

    @Test
    void nativeWritableBackingCommitsInPlaceWithoutStaging() throws Exception {
        try (CompiledSpec spec = GoldensTest.spec("text-bytes");
             Builder builder = Builder.create(spec);
             Backing backing = new Backing(null)) {
            GoldenScenarios.author(spec, builder, "text_bytes");
            try (BuildPlan plan = builder.freeze();
                 BuildPlan retained = plan.retain();
                 ExecutionResult heap = plan.execute(BuildPolicy.REQUIRE_DIRECT);
                 ExecutionResult nativeBuild = retained.executeExternalNative(BuildPolicy.REQUIRE_DIRECT,
                         new NativeWritableBacking(tableAddress(backing.address())))) {
                assertEquals(ExecutionMode.DIRECT, nativeBuild.report().mode());
                assertEquals(FallbackReason.NONE, nativeBuild.report().fallbackReason());
                assertEquals(0, nativeBuild.report().stagingBytes());
                assertArrayEquals(new long[] {0, 0, 1, 1, 0}, counts(backing.address()));
                try (Access access = nativeBuild.payload().acquire()) {
                    assertEquals(dataAddress(backing.address()), access.dataAddress());
                    assertArrayEquals(heap.payload().binaryBytes(), access.toByteArray());
                }
                nativeBuild.payload().invalidate();
                assertArrayEquals(new long[] {0, 1, 1, 1, 0}, counts(backing.address()));
            }
        }
    }

    private static native long create(byte[] bytes);
    private static native void destroy(long address);
    private static native long[] bytesBacking(long address);
    private static native long tableAddress(long address);
    private static native long[] counts(long address);
    private static native long dataAddress(long address);
    static native long directAddress(ByteBuffer buffer);
    private static native long[] invalidateDetached(long payload);

    private static final class Backing extends NativeHandle {
        Backing(byte[] bytes) {
            super(NativeBackingTest::destroy);
            adopt(create(bytes));
        }

        NativeBytesBacking bytes() {
            long[] raw = bytesBacking(address());
            return new NativeBytesBacking(raw[0], raw[1], raw[2], raw[3], raw[4]);
        }
    }
}
