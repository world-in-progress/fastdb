package io.fastdb.payload;

import static org.junit.jupiter.api.Assertions.*;

import java.nio.BufferOverflowException;
import java.nio.ByteBuffer;
import java.nio.ReadOnlyBufferException;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.HexFormat;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.locks.LockSupport;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.function.Executable;

final class RuntimeBoundariesTest {
    @Test
    void largeDirectSpansRemainPinnedAndCopyIntoReusableBuffers() {
        int size = 16 * 1024 * 1024;
        ByteBuffer input = ByteBuffer.allocateDirect(size + 16).position(2).slice();
        input.position(7).limit(7 + size);
        var ints = input.asIntBuffer();
        for (int i = 0; i < ints.capacity(); i++) {
            ints.put(i, i * 73);
        }

        try (CompiledSpec spec = scalarSpec("bytes"); BuildPlan plan = bytesPlan(spec, input);
             ExecutionResult built = plan.execute(BuildPolicy.REQUIRE_DIRECT);
             Access image = built.payload().acquire()) {
            assertEquals(7, input.position());
            assertEquals(7 + size, input.limit());
            assertEquals(size, plan.info().opaqueBytes());
            assertEquals(0, built.report().stagingBytes());

            int imageLength = Math.toIntExact(image.byteLength());
            ByteBuffer storage = ByteBuffer.allocateDirect(imageLength + 80);
            storage.position(8);
            ByteBuffer external = storage.slice().position(56).limit(56 + imageLength);
            image.copyTo(external);
            assertEquals(56 + imageLength, external.position());
            external.position(56);
            try (Payload opened = Payload.openExternal(spec, external.asReadOnlyBuffer(), OpenOptions.defaults());
                 Payload copied = Payload.openCopy(spec, external);
                 Access pin = opened.acquire();
                 View entry = opened.entryView(0);
                 View value = entry.at(0);
                 Access bytes = value.acquire()) {
                assertEquals(56, external.position());
                assertEquals(NativeBackingTest.directAddress(external) + 56, pin.dataAddress());
                try (Access copiedPin = copied.acquire()) {
                    assertNotEquals(pin.dataAddress(), copiedPin.dataAddress());
                    assertEquals(-1, pin.unsafeByteBuffer().mismatch(copiedPin.unsafeByteBuffer()));
                }
                ByteBuffer alias = bytes.unsafeByteBuffer();
                assertTrue(alias.isReadOnly());
                assertEquals(bytes.dataAddress(), NativeBackingTest.directAddress(alias));
                assertEquals(size, bytes.byteLength());
                assertEquals(-1, input.mismatch(alias));

                ByteBuffer destination = ByteBuffer.allocateDirect(size + 16);
                destination.put(2, (byte) 0x12).put(size + 3, (byte) 0x34);
                destination.position(3);
                value.copyBytesInto(destination);
                assertEquals(3 + size, destination.position());
                assertEquals(0x12, destination.get(2));
                assertEquals(0x34, destination.get(size + 3));
                assertEquals(-1, input.mismatch(destination.position(3).limit(3 + size)));
                destination.limit(3 + size - 1);
                assertThrows(BufferOverflowException.class, () -> value.copyBytesInto(destination));
                assertEquals(3, destination.position());
                assertThrows(ReadOnlyBufferException.class, () -> bytes.copyTo(destination.asReadOnlyBuffer()));
                assertThrows(ReadOnlyBufferException.class, () -> value.copyBytesInto(destination.asReadOnlyBuffer()));
            }
        }
    }

    @Test
    void slicesAndEmptyByteArraysRespectPositionsAndLengths() {
        byte[] source = {9, 8, 7, 6, 5, 4};
        ByteBuffer heap = ByteBuffer.wrap(source).position(1).slice().position(1).limit(4);
        try (CompiledSpec spec = scalarSpec("bytes"); Builder builder = Builder.create(spec)) {
            builder.entryBegin(0, 4);
            assertThrows(IndexOutOfBoundsException.class, () -> builder.valueBytes(source, -1, 2));
            assertThrows(IndexOutOfBoundsException.class, () -> builder.valueBytes(source, 4, Integer.MAX_VALUE));
            builder.valueBytes(source, 1, 3).valueBytes(heap).valueBytes(heap.asReadOnlyBuffer());
            builder.valueBytes(ByteBuffer.allocateDirect(0));
            assertEquals(1, heap.position());
            try (BuildPlan plan = builder.freeze();
                 ExecutionResult result = plan.execute(BuildPolicy.ALLOW_STAGING);
                 View entry = result.payload().entryView(0);
                 View slice = entry.at(0);
                 View buffer = entry.at(1);
                 View readOnly = entry.at(2);
                 View empty = entry.at(3);
                 Access emptyPin = empty.acquire()) {
                assertArrayEquals(new byte[] {8, 7, 6}, slice.getBytes());
                assertArrayEquals(new byte[] {7, 6, 5}, buffer.getBytes());
                assertArrayEquals(buffer.getBytes(), readOnly.getBytes());
                assertEquals(0, emptyPin.unsafeByteBuffer().capacity());

                byte[] target = new byte[7];
                ByteBuffer output = ByteBuffer.wrap(target).position(1).slice().position(1).limit(4);
                buffer.copyBytesInto(output);
                assertEquals(4, output.position());
                assertArrayEquals(new byte[] {0, 0, 7, 6, 5, 0, 0}, target);

                byte[] bytes = result.payload().binaryBytes();
                ByteBuffer image = ByteBuffer.allocate(bytes.length + 4).position(2).put(bytes).flip().position(2);
                try (Payload copied = Payload.openCopy(spec, image.asReadOnlyBuffer())) {
                    assertArrayEquals(bytes, copied.binaryBytes());
                    assertEquals(2, image.position());
                }
                assertThrows(IllegalArgumentException.class, () -> Payload.openExternal(spec, image).close());
            }
        }
    }

    @Test
    void invalidationDrainsAConcurrentAccessAndRejectsAllNewReads() throws Exception {
        try (CompiledSpec spec = scalarSpec("bytes"); Builder builder = Builder.create(spec)) {
            builder.entryBegin(0, 1).valueBytes(new byte[] {1, 2, 3});
            try (BuildPlan plan = builder.freeze();
                 ExecutionResult result = plan.execute(BuildPolicy.REQUIRE_DIRECT);
                 Payload retained = result.payload().retain();
                 View sequence = result.payload().entryView(0);
                 View original = sequence.at(0);
                 View value = original.retain();
                 View detached = value.materialize()) {
                var entered = new CountDownLatch(1);
                var release = new CountDownLatch(1);
                var threads = Executors.newFixedThreadPool(2);
                try {
                    var reader = threads.submit(() -> {
                        try (Access access = value.acquire()) {
                            entered.countDown();
                            assertTrue(release.await(10, TimeUnit.SECONDS));
                            assertArrayEquals(new byte[] {1, 2, 3}, access.toByteArray());
                        }
                        return null;
                    });
                    assertTrue(entered.await(5, TimeUnit.SECONDS));
                    var invalidator = threads.submit(retained::invalidate);
                    long until = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
                    boolean rejected = false;
                    while (System.nanoTime() < until) {
                        try {
                            sequence.length();
                        } catch (FastDbException error) {
                            assertEquals(FastDbException.E_VIEW_INVALIDATED, error.code());
                            rejected = true;
                            break;
                        }
                        LockSupport.parkNanos(TimeUnit.MILLISECONDS.toNanos(1));
                    }
                    assertTrue(rejected, "barrier did not reject new reads");
                    assertThrows(TimeoutException.class, () -> invalidator.get(50, TimeUnit.MILLISECONDS));
                    assertCode(FastDbException.E_VIEW_INVALIDATED, value::kind);
                    assertCode(FastDbException.E_VIEW_INVALIDATED, () -> value.copyBytesInto(ByteBuffer.allocate(3)));
                    assertCode(FastDbException.E_VIEW_INVALIDATED, value::materialize);
                    assertCode(FastDbException.E_VIEW_INVALIDATED, result.payload()::binaryBytes);
                    release.countDown();
                    reader.get(5, TimeUnit.SECONDS);
                    invalidator.get(5, TimeUnit.SECONDS);
                    result.payload().invalidate();
                    result.payload().close();
                    assertArrayEquals(new byte[] {1, 2, 3}, detached.getBytes());
                } finally {
                    release.countDown();
                    threads.shutdownNow();
                    assertTrue(threads.awaitTermination(10, TimeUnit.SECONDS));
                }
            }
        }
    }

    @Test
    void coreRejectsMalformedUtf8AndIsolatedUtf16Surrogates() {
        String text = "𐐷\0中";
        try (CompiledSpec spec = scalarSpec("str"); Builder builder = Builder.create(spec)) {
            builder.entryBegin(0, 1);
            assertCode(FastDbException.E_INVALID_TEXT_ENCODING, () -> builder.valueStr("\ud800"));
            assertCode(FastDbException.E_INVALID_TEXT_ENCODING, () -> builder.valueStr("\udc00"));
            builder.valueStr(text);
            try (BuildPlan plan = builder.freeze();
                 ExecutionResult result = plan.execute(BuildPolicy.REQUIRE_DIRECT);
                 View entry = result.payload().entryView(0);
                 View value = entry.at(0);
                 Access access = value.acquire()) {
                assertEquals(text, value.getStr());
                assertEquals(text, access.asStr());
                byte[] encoded = text.getBytes(StandardCharsets.UTF_8);
                assertArrayEquals(encoded, access.toByteArray());
                byte[] corrupted = result.payload().binaryBytes();
                String image = HexFormat.of().formatHex(corrupted);
                int offset = image.lastIndexOf(HexFormat.of().formatHex(encoded));
                assertTrue(offset >= 0 && offset % 2 == 0);
                corrupted[offset / 2] = (byte) 0xff;
                assertCode(FastDbException.E_INVALID_TEXT_ENCODING, () -> Payload.openCopy(spec, corrupted,
                        OpenOptions.defaults().withEagerTextValidation(true)).close());
                try (Payload opened = Payload.openCopy(spec, corrupted,
                        OpenOptions.defaults().withEagerTextValidation(false));
                     View openedEntry = opened.entryView(0); View openedText = openedEntry.at(0)) {
                    assertCode(FastDbException.E_INVALID_TEXT_ENCODING, openedText::getStr);
                }
            }
        }

        try (CompiledSpec spec = scalarSpec("wstr"); Builder builder = Builder.create(spec)) {
            builder.entryBegin(0, 1);
            assertCode(FastDbException.E_INVALID_TEXT_ENCODING, () -> builder.valueWstr("\ud800"));
            assertCode(FastDbException.E_INVALID_TEXT_ENCODING, () -> builder.valueWstr("\udc00"));
            builder.valueWstr(text);
            try (BuildPlan plan = builder.freeze();
                 ExecutionResult result = plan.execute(BuildPolicy.REQUIRE_DIRECT);
                 View entry = result.payload().entryView(0);
                 View value = entry.at(0);
                 Access access = value.acquire()) {
                assertEquals(text, value.getWstr());
                assertEquals(text, access.asWstr());
                assertEquals(text.length() * 2L, access.byteLength());
                assertCode(FastDbException.E_TYPE_MISMATCH, value::getStr);
            }
        }
    }

    @Test
    void unsignedChecksRejectBeforeChangingBuilderState() throws Exception {
        try (CompiledSpec spec = GoldensTest.spec("fixed-scalars");
             Builder builder = Builder.create(spec)) {
            assertCode(FastDbException.E_OUT_OF_RANGE, () -> builder.valueU8(-1));
            assertCode(FastDbException.E_OUT_OF_RANGE, () -> builder.valueU8(256));
            assertCode(FastDbException.E_OUT_OF_RANGE, () -> builder.valueU16(-1));
            assertCode(FastDbException.E_OUT_OF_RANGE, () -> builder.valueU16(65536));
            assertCode(FastDbException.E_OUT_OF_RANGE, () -> builder.valueU32(-1));
            assertCode(FastDbException.E_OUT_OF_RANGE, () -> builder.valueU32(1L << 32));
            GoldenScenarios.author(spec, builder, "fixed_scalars");
            try (BuildPlan plan = builder.freeze();
                 ExecutionResult result = plan.execute(BuildPolicy.ALLOW_STAGING)) {
                assertArrayEquals(GoldensTest.binary("fixed-scalars"), result.payload().binaryBytes());
            }
        }
    }

    @Test
    void schemaIdentityAndIndependentlyRetainedHandles() throws Exception {
        assertArrayEquals(MessageDigest.getInstance("SHA-256").digest(CompiledSpec.sourceSchemaJson()),
                CompiledSpec.sourceSchemaSha256());
        CompiledSpec original = scalarSpec("bytes");
        try (CompiledSpec retained = original.retain();
             CompiledSpec wrong = scalarSpec("str"); Builder builder = Builder.create(retained)) {
            original.close();
            original.close();
            assertThrows(IllegalStateException.class, original::sha256);
            assertCode(FastDbException.E_DIGEST_MISMATCH, () -> builder.requireSpecSha256(wrong.sha256()));
            builder.requireSpecSha256(retained.sha256());
            builder.entryBegin(0, 1).valueBytes(new byte[] {1});
            try (BuildPlan plan = builder.freeze();
                 ExecutionResult result = plan.execute(BuildPolicy.REQUIRE_DIRECT);
                 View view = result.payload().entryView(0)) {
                assertCode(FastDbException.E_DIGEST_MISMATCH, () -> result.payload().requireSpecSha256(wrong.sha256()));
                assertCode(FastDbException.E_DIGEST_MISMATCH, () -> view.requireSpecSha256(wrong.sha256()));
                view.requireSpecSha256(retained.sha256());
            }
        } finally {
            original.close();
        }
    }

    private static BuildPlan bytesPlan(CompiledSpec spec, ByteBuffer bytes) {
        try (Builder builder = Builder.create(spec)) {
            return builder.entryBegin(0, 1).valueBytes(bytes).freeze();
        }
    }

    private static CompiledSpec scalarSpec(String kind) {
        return CompiledSpec.compile("""
                {"schema":"fastdb.payload.v1","profile":"record.v1","components":[],
                 "entries":[{"id":"value","cardinality":"many","type":{"kind":"%s"}}]}
                """.formatted(kind));
    }

    private static void assertCode(int expected, Executable operation) {
        FastDbException failure = assertThrows(FastDbException.class, operation);
        assertEquals(expected, failure.code(), failure::getMessage);
    }
}
