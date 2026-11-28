package io.fastdb.payload;

import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.util.Locale;

final class FastDbNative {
    static {
        load();
    }

    private FastDbNative() {}

    private static void load() {
        String path = System.getProperty("fastdb.java.library.path");
        if (path == null || path.isBlank()) {
            path = System.getenv("FASTDB_JAVA_LIBRARY_PATH");
        }
        if (path != null && !path.isBlank()) {
            System.load(Path.of(path).toAbsolutePath().normalize().toString());
            return;
        }

        String platform = platform();
        if (platform != null) {
            String name = System.mapLibraryName("fastdb_java");
            String resource = "/META-INF/native/" + platform + "/" + name;
            try (InputStream library = FastDbNative.class.getResourceAsStream(resource)) {
                if (library != null) {
                    Path directory = Files.createTempDirectory("fastdb-java-");
                    directory.toFile().deleteOnExit();
                    Path extracted = directory.resolve(name);
                    extracted.toFile().deleteOnExit();
                    Files.copy(library, extracted, StandardCopyOption.REPLACE_EXISTING);
                    System.load(extracted.toString());
                    return;
                }
            } catch (IOException failure) {
                UnsatisfiedLinkError error = new UnsatisfiedLinkError("Cannot extract " + resource);
                error.initCause(failure);
                throw error;
            }
        }
        System.loadLibrary("fastdb_java");
    }

    private static String platform() {
        String os = System.getProperty("os.name").toLowerCase(Locale.ROOT);
        String arch = System.getProperty("os.arch").toLowerCase(Locale.ROOT);
        String machine = switch (arch) {
            case "amd64", "x86_64" -> "x86_64";
            case "aarch64", "arm64" -> "arm64";
            default -> null;
        };
        if (machine == null) {
            return null;
        }
        if (os.startsWith("linux")) {
            return "linux-" + machine;
        }
        if (os.startsWith("mac") || os.startsWith("darwin")) {
            return "macos-" + machine;
        }
        if (os.startsWith("windows")) {
            return "windows-" + machine;
        }
        return null;
    }

    static native long[] compileOptions();
    static native long[] codegenOptions();
    static native long specCompile(byte[] source, int length, long[] raw);
    static native long specRetain(long handle);
    static native void specRelease(long handle);
    static native void artifactResultRelease(long handle);
    static native byte[] specSha256(long handle);
    static native byte[] specCanonicalJson(long handle);
    static native byte[] specManifestJson(long handle);
    static native int specProfile(long handle);
    static native long[] specCapabilities(long handle);
    static native int specEntryCount(long handle);
    static native byte[] specEntryId(long handle, int index);
    static native int specEntryIndex(long handle, byte[] id);
    static native int specComponentCount(long handle);
    static native byte[] specComponentId(long handle, int index);
    static native int specComponentIndex(long handle, byte[] id);
    static native int specComponentFieldCount(long handle, int component);
    static native byte[] specComponentFieldId(long handle, int component, int index);
    static native int specComponentFieldIndex(long handle, int component, byte[] id);
    static native byte[] sourceSchemaJson();
    static native byte[] sourceSchemaSha256();
    static native long specGenerate(long handle, long target, long[] raw);
    static native long artifactCount(long handle);
    static native byte[] artifactPath(long handle, long index);
    static native byte[] artifactBytes(long handle, long index);
    static native int artifactKind(long handle, long index);
    static native byte[] artifactSha256(long handle, long index);

    static native long[] builderOptions();
    static native long builderCreate(long handle, long[] raw);
    static native void builderRelease(long handle);
    static native void builderRequireDigest(long handle, byte[] expected);
    static native void builderEntryBegin(long handle, int index, long count);
    static native long builderDeclareObject(long handle, int component);
    static native void builderValueNull(long handle);
    static native void builderValueBool(long handle, boolean value);
    static native void builderValueU8(long handle, int value);
    static native void builderValueU16(long handle, int value);
    static native void builderValueU32(long handle, long value);
    static native void builderValueI32(long handle, int value);
    static native void builderValueU8n(long handle, long value);
    static native void builderValueU16n(long handle, long value);
    static native void builderValueF32(long handle, int value);
    static native void builderValueF64(long handle, long value);
    static native void builderValueComponentBegin(long handle);
    static native void builderValueListBegin(long handle, long value);
    static native void builderObjectFillBegin(long handle, long value);
    static native void builderValueObject(long handle, long value);
    static native void builderValueRef(long handle, long value);
    static native void builderValueStr(long handle, byte[] value);
    static native void builderValueWstr(long handle, String value);
    static native void builderValueBytes(long handle, byte[] value, int offset, int length);
    static native void builderValueDirectBytes(long handle, ByteBuffer buffer, int offset, int length);
    static native long builderFreeze(long handle);
    static native long planRetain(long handle);
    static native void planRelease(long handle);
    static native long[] planInfo(long handle);
    static native long planExecute(long handle, int policy, long table, long[] out_report);

    static native long[] openOptions();
    static native long payloadOpenArray(long handle, byte[] value, int offset, int length, long[] raw);
    static native long payloadOpenDirect(long handle, ByteBuffer buffer, int offset, int length, long[] raw, boolean external);
    static native long payloadOpenNative(long handle, long data, long length, long retain, long release, long owner, long[] raw);
    static native long payloadRetain(long handle);
    static native void payloadRelease(long handle);
    static native void payloadRequireDigest(long handle, byte[] expected);
    static native byte[] payloadSha256(long handle);
    static native int payloadProfile(long handle);
    static native long[] payloadReport(long handle);
    static native long payloadAcquire(long handle);
    static native long payloadEntryView(long handle, int index);
    static native void payloadInvalidate(long handle);

    static native long viewRetain(long handle);
    static native void viewRelease(long handle);
    static native void viewRequireDigest(long handle, byte[] expected);
    static native int viewKind(long handle);
    static native boolean viewIsNull(long handle);
    static native long viewLength(long handle);
    static native int viewComponentIndex(long handle);
    static native int viewFieldCount(long handle);
    static native boolean viewGetBool(long handle);
    static native int viewGetU8(long handle);
    static native int viewGetU16(long handle);
    static native long viewGetU32(long handle);
    static native int viewGetI32(long handle);
    static native long viewGetU8n(long handle);
    static native long viewGetU16n(long handle);
    static native int viewGetF32(long handle);
    static native long viewGetF64(long handle);
    static native long viewAt(long handle, long index);
    static native long viewField(long handle, int index);
    static native long viewRefTarget(long handle);
    static native long viewMaterialize(long handle);
    static native long[] viewGraphIdentity(long handle);
    static native long viewAcquire(long handle);
    static native int viewCopyBytes(long handle, ByteBuffer direct, byte[] array, int offset, int remaining);

    static native void accessRelease(long handle);
    static native long accessByteLength(long handle, int kind);
    static native long accessDataAddress(long handle, int kind);
    static native byte[] accessToArray(long handle, int kind);
    static native ByteBuffer accessDirectBuffer(long handle, int kind);
    static native int accessCopy(long handle, int kind, ByteBuffer direct, byte[] array, int offset, int remaining);
    static native String accessWstr(long handle);
}
