package io.fastdb.payload;

import java.util.Objects;

/** A structured failure reported by the FastDB C ABI or the Java binding. */
public final class FastDbException extends RuntimeException {
    private static final long serialVersionUID = 1L;

    public static final int E_INVALID_JSON = 1001;
    public static final int E_DUPLICATE_KEY = 1002;
    public static final int E_UNKNOWN_FIELD = 1003;
    public static final int E_UNSUPPORTED_SCHEMA = 1004;
    public static final int E_INVALID_TYPE = 1005;
    public static final int E_DUPLICATE_ID = 1006;
    public static final int E_UNRESOLVED_COMPONENT = 1007;
    public static final int E_PROFILE_VIOLATION = 1008;
    public static final int E_INVALID_NUMBER = 1009;
    public static final int E_SPEC_RESOURCE_LIMIT = 1010;
    public static final int E_MISSING_ENTRY = 2001;
    public static final int E_MISSING_FIELD = 2002;
    public static final int E_UNEXPECTED_NULL = 2003;
    public static final int E_TYPE_MISMATCH = 2004;
    public static final int E_OUT_OF_RANGE = 2005;
    public static final int E_BUILDER_STATE = 2006;
    public static final int E_DIRECT_UNAVAILABLE = 2007;
    public static final int E_PLAN_STATE = 2008;
    public static final int E_RUNTIME_UNAVAILABLE = 2009;
    public static final int E_INVALID_TEXT_ENCODING = 2010;
    public static final int E_BUILDER_LENGTH_OVERFLOW = 2011;
    public static final int E_BUILDER_OUT_OF_BOUNDS = 2012;
    public static final int E_BUILDER_RESOURCE_LIMIT = 2013;
    public static final int E_INVALID_OBJECT_HANDLE = 2014;
    public static final int E_UNREACHABLE_OBJECT = 2015;
    public static final int E_INVALID_MAGIC = 3001;
    public static final int E_UNSUPPORTED_BINARY_VERSION = 3002;
    public static final int E_LENGTH_OVERFLOW = 3003;
    public static final int E_OUT_OF_BOUNDS = 3004;
    public static final int E_MISALIGNED = 3005;
    public static final int E_DIGEST_MISMATCH = 3006;
    public static final int E_INVALID_REFERENCE = 3007;
    public static final int E_RESOURCE_LIMIT = 3008;
    public static final int E_NON_CANONICAL_BINARY = 3009;
    public static final int E_INVALID_BINARY_VALUE = 3010;
    public static final int E_VIEW_INVALIDATED = 4001;
    public static final int E_STALE_GENERATION = 4002;
    public static final int E_READ_ONLY = 4003;
    public static final int E_BACKING_CONTRACT = 5001;
    public static final int E_ALLOCATION_FAILED = 5002;
    public static final int E_COMMIT_FAILED = 5003;
    public static final int E_ROLLBACK_FAILED = 5004;
    public static final int E_UNSUPPORTED_TARGET = 6001;
    public static final int E_INVALID_ARTIFACT_PATH = 6002;
    public static final int E_GENERATOR_FAILED = 6003;
    public static final int E_INVALID_ARGUMENT = 7001;
    public static final int E_UNSUPPORTED_ABI = 7002;
    public static final int E_NOT_FOUND = 7003;
    public static final int E_INDEX_OUT_OF_RANGE = 7004;
    public static final int E_INTERNAL = 9001;

    private final int code;
    private final String symbol;
    private final String path;
    private final String detailsJson;

    public FastDbException(int code, String symbol, String path, String message, String detailsJson) {
        super(message);
        this.code = code;
        this.symbol = Objects.requireNonNull(symbol);
        this.path = Objects.requireNonNull(path);
        this.detailsJson = Objects.requireNonNull(detailsJson);
    }

    // Core returns standard UTF-8 for every error field.
    private FastDbException(int code, byte[] symbol, byte[] path, byte[] message, byte[] detailsJson) {
        this(code, NativeValues.text(symbol), NativeValues.text(path),
                NativeValues.text(message), NativeValues.text(detailsJson));
    }

    public int code() { return code; }
    public String symbol() { return symbol; }
    public String path() { return path; }
    public String message() { return getMessage(); }
    public String detailsJson() { return detailsJson; }
}
