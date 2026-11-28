# FastDB for Java

`io.github.world-in-progress:fastdb-java:0.4.0` projects the portable payload C ABI into
Java 17 and later. Core compiles specs, builds and reads payloads, and generates
source for all five targets. The Java JAR has no runtime dependencies.

The Maven Central release JAR bundles JNI libraries for Linux x86_64 (built on
Ubuntu 24.04, glibc 2.34+), macOS arm64 (macOS 11+), and Windows x64. Add the
dependency and import the API from `io.fastdb.payload`.

```xml
<dependency>
    <groupId>io.github.world-in-progress</groupId>
    <artifactId>fastdb-java</artifactId>
    <version>0.4.0</version>
</dependency>
```

## Build from source

From the repository root, set `JAVA_HOME` to a JDK and build the JNI library and
its test fixture.

```sh
cmake -S fastcarto -B bindings/java/target/cmake -DCMAKE_BUILD_TYPE=Release -DFASTDB_BUILD_JAVA_JNI=ON -DBUILD_TESTING=ON
cmake --build bindings/java/target/cmake --config Release --parallel
ctest --test-dir bindings/java/target/cmake -C Release --output-on-failure
mvn -f bindings/java/pom.xml -o test -Dfastdb.java.library.path="$PWD/bindings/java/target/cmake/fastdb/java/libfastdb_java.dylib"
```

The example uses macOS arm64. On Linux x86_64 the library is
`libfastdb_java.so`. On Windows x64 use `fastdb_java.dll` from the corresponding
CMake configuration directory. Offline Maven requires the compiler, resource,
JAR, and Surefire plugins and their dependencies in the local cache, including
Surefire's JUnit Platform provider and launcher. Use
`mvn -f bindings/java/pom.xml package -DskipTests` after tests pass (or pass the
same `-Dfastdb.java.library.path=...` property) to build the JAR. Build the native library with `BUILD_TESTING=OFF`
when packaging for distribution.

At runtime, the loader checks the exact library path in
`fastdb.java.library.path`, then `FASTDB_JAVA_LIBRARY_PATH`, then
`META-INF/native/<os>-<arch>/<library-name>` on the classpath. Platform directory
names include `macos-arm64`, `linux-x86_64`, and `windows-x86_64`. The final
lookup uses the JVM's library search path.

Use try-with-resources for owning objects and access pins. A retained wrapper
owns its own native reference. `Payload.invalidate()` stops new access, waits
for open pins, and releases external backing. Close a pin before invalidating
the same payload on the thread that owns the pin.

Builder byte input and payload open methods consume the input buffer's
remaining range without moving its position. Direct builder input stays pinned
through builder and plan use. External payload input stays pinned until
invalidation or backing release. Keep both pinned ranges unchanged.
`Access.copyTo` and `View.copyBytesInto` advance the destination position on
success. Keep any buffer from `Access.unsafeByteBuffer()` or address from
`Access.dataAddress()` within the lifetime of that access.

`NativeBytesBacking` and `NativeWritableBacking` accept in-process addresses
from C ABI integrations. Their Java documentation describes which memory and
callbacks the caller must keep alive.
