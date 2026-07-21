import { initPayloadModule } from './abi.js';

export {
  BuildPlan,
  Builder,
  FixedRun,
  ObjectHandle,
  PlanInfo,
} from './builder.js';
export type { BuilderOptions } from './builder.js';
export { PayloadError } from './error.js';
export { Capabilities, CompiledSpec, Profile } from './spec.js';
export {
  BuildPolicy,
  BuildResult,
  ExecutionMode,
  ExecutionReport,
  FallbackReason,
  OpenOptions,
  Payload,
  WasmMemoryBacking,
  WasmOwnedBytes,
} from './runtime.js';
export type {
  OpenOptionValues,
  PayloadBackingStats,
  WasmMemoryBackingOptions,
} from './runtime.js';

export async function initPayload(): Promise<void> {
  await initPayloadModule();
}
