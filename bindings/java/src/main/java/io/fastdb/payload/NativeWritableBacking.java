package io.fastdb.payload;

/**
 * Address of a C fdb_payload_v1_backing_v1_t table. Core copies the table
 * during execution. Its context and callbacks must stay valid until Core
 * rolls back or releases the backing. Serialize use of a shared context.
 */
public record NativeWritableBacking(long tableAddress) {
    public NativeWritableBacking {
        if (tableAddress == 0) {
            throw new IllegalArgumentException("Expected a C backing table address");
        }
    }
}
