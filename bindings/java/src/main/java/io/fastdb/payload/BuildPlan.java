package io.fastdb.payload;

import java.util.Objects;

/** Immutable, repeatable plan. Executions may run concurrently on distinct backings. */
public final class BuildPlan extends NativeHandle {
    BuildPlan() {
        super(FastDbNative::planRelease);
    }

    public synchronized BuildPlan retain() {
        BuildPlan result = new BuildPlan();
        result.adopt(FastDbNative.planRetain(address()));
        return result;
    }

    public synchronized PlanInfo info() {
        return PlanInfo.fromNative(FastDbNative.planInfo(address()));
    }

    public ExecutionResult execute(BuildPolicy policy) {
        return execute(policy, 0);
    }

    public ExecutionResult executeExternalNative(BuildPolicy policy, NativeWritableBacking backing) {
        return execute(policy, backing.tableAddress());
    }

    private ExecutionResult execute(BuildPolicy policy, long backing) {
        Objects.requireNonNull(policy);
        Payload payload = new Payload();
        long[] report = new long[7];
        try (BuildPlan live = retain()) {
            payload.adopt(FastDbNative.planExecute(live.address(), policy.code(), backing, report));
            return new ExecutionResult(payload, ExecutionReport.fromNative(report));
        } catch (RuntimeException | Error failure) {
            payload.close();
            throw failure;
        }
    }
}
