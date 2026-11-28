import io.fastdb.payload.Builder;
import io.fastdb.payload.BuildPolicy;
import io.fastdb.payload.CompiledSpec;
import io.fastdb.payload.Payload;
import java.security.MessageDigest;
import java.util.Arrays;

public class JavaReleaseSmoke {
    public static void main(String[] args) throws Exception {
        byte[] schemaDigest = MessageDigest.getInstance("SHA-256").digest(CompiledSpec.sourceSchemaJson());
        if (!Arrays.equals(schemaDigest, CompiledSpec.sourceSchemaSha256())) {
            throw new AssertionError("Packaged JNI returned an unexpected schema digest");
        }
        try (var spec = CompiledSpec.compile("""
                {"schema":"fastdb.payload.v1","profile":"record.v1","components":[],
                 "entries":[{"id":"value","cardinality":"many","type":{"kind":"i32"}}]}
                """);
             var builder = Builder.create(spec);
             var plan = builder.entryBegin(0, 1).valueI32(42).freeze();
             var result = plan.execute(BuildPolicy.ALLOW_STAGING);
             var reopened = Payload.openCopy(spec, result.payload().binaryBytes());
             var entry = reopened.entryView(0);
             var value = entry.at(0)) {
            if (value.getI32() != 42) {
                throw new AssertionError("Packaged JNI returned an unexpected payload value");
            }
        }
    }
}
