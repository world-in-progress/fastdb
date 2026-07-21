import { initPayloadModule } from './abi.js';

export { PayloadError } from './error.js';
export { Capabilities, CompiledSpec, Profile } from './spec.js';

export async function initPayload(): Promise<void> {
  await initPayloadModule();
}
