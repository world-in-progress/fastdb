import {
  CAPABILITIES_SIZE,
  POINTER_SIZE,
  SHA256_SIZE,
  checkedU32,
  copyBytes,
  payloadModule,
  readU32,
  readU64,
  withAllocation,
  withInputBytes,
} from './abi.js';
import { PayloadError, bindingError, checkStatus } from './error.js';

export enum Profile {
  RecordV1 = 1,
  ObjectGraphV1 = 2,
}

export class Capabilities {
  constructor(
    readonly profile: Profile,
    readonly semanticFlags: bigint,
    readonly operationFlags: bigint,
    readonly codegenTargetFlags: bigint,
    readonly directBuildStatus: number,
  ) {}
}

const encoder = new TextEncoder();
const decoder = new TextDecoder('utf-8', { fatal: true });

const specFinalizer = new FinalizationRegistry<number>((handle) => {
  try {
    payloadModule()._fdb_payload_v1_spec_release(handle);
  } catch {
    // Explicit dispose is authoritative; finalization is only a fallback.
  }
});

type BlobQuery = (spec: number, outBlob: number, outError: number) => number;
type CountQuery = (spec: number, outCount: number, outError: number) => number;
type IndexedIdQuery = (
  spec: number,
  index: number,
  outId: number,
  outError: number,
) => number;
type NamedIndexQuery = (
  spec: number,
  id: number,
  idSize: bigint,
  outIndex: number,
  outError: number,
) => number;

export class CompiledSpec {
  private handle: number;
  private readonly finalizerToken = {};

  private constructor(handle: number) {
    if (handle === 0) {
      throw bindingError(
        'FastDB Core returned success without a compiled spec',
        'missing_spec_handle',
      );
    }
    this.handle = handle;
    specFinalizer.register(this, handle, this.finalizerToken);
  }

  static compile(source: Uint8Array): CompiledSpec {
    const module = payloadModule();
    return withInputBytes(module, source, (sourcePointer, sourceSize) =>
      withAllocation(module, POINTER_SIZE * 2, (outputs) => {
        const status = module._fdb_payload_v1_spec_compile_json(
          sourcePointer,
          sourceSize,
          0,
          outputs,
          outputs + POINTER_SIZE,
        );
        checkStatus(status, outputs + POINTER_SIZE);
        return new CompiledSpec(readU32(module, outputs));
      }),
    );
  }

  clone(): CompiledSpec {
    const handle = this.requireHandle();
    payloadModule()._fdb_payload_v1_spec_retain(handle);
    return new CompiledSpec(handle);
  }

  dispose(): void {
    if (this.handle !== 0) {
      const handle = this.handle;
      this.handle = 0;
      specFinalizer.unregister(this.finalizerToken);
      payloadModule()._fdb_payload_v1_spec_release(handle);
    }
  }

  canonicalJson(): Uint8Array {
    const module = payloadModule();
    return this.queryBlob(
      module._fdb_payload_v1_spec_canonical_json.bind(module),
    );
  }

  manifestJson(): Uint8Array {
    const module = payloadModule();
    return this.queryBlob(
      module._fdb_payload_v1_spec_manifest_json.bind(module),
    );
  }

  sha256(): Uint8Array {
    const module = payloadModule();
    return withAllocation(module, SHA256_SIZE + POINTER_SIZE, (outputs) => {
      const status = module._fdb_payload_v1_spec_sha256(
        this.requireHandle(),
        outputs,
        outputs + SHA256_SIZE,
      );
      checkStatus(status, outputs + SHA256_SIZE);
      return module.HEAPU8.slice(outputs, outputs + SHA256_SIZE);
    });
  }

  profile(): Profile {
    const module = payloadModule();
    return withAllocation(module, POINTER_SIZE * 2, (outputs) => {
      const status = module._fdb_payload_v1_spec_profile(
        this.requireHandle(),
        outputs,
        outputs + POINTER_SIZE,
      );
      checkStatus(status, outputs + POINTER_SIZE);
      return checkedProfile(readU32(module, outputs));
    });
  }

  capabilities(): Capabilities {
    const module = payloadModule();
    return withAllocation(
      module,
      CAPABILITIES_SIZE + POINTER_SIZE,
      (outputs) => {
        module._fdb_payload_v1_capabilities_init(outputs);
        const status = module._fdb_payload_v1_spec_capabilities(
          this.requireHandle(),
          outputs,
          outputs + CAPABILITIES_SIZE,
        );
        checkStatus(status, outputs + CAPABILITIES_SIZE);
        return new Capabilities(
          checkedProfile(readU32(module, outputs + 4)),
          readU64(module, outputs + 8),
          readU64(module, outputs + 16),
          readU64(module, outputs + 24),
          readU32(module, outputs + 32),
        );
      },
    );
  }

  entryCount(): number {
    const module = payloadModule();
    return this.queryCount(module._fdb_payload_v1_spec_entry_count.bind(module));
  }

  entryId(index: number): string {
    const module = payloadModule();
    return this.queryIndexedId(
      module._fdb_payload_v1_spec_entry_id.bind(module),
      checkedU32(index, 'entryIndex'),
    );
  }

  entryIndex(identifier: string): number {
    const module = payloadModule();
    return this.queryNamedIndex(
      module._fdb_payload_v1_spec_entry_index.bind(module),
      identifier,
    );
  }

  componentCount(): number {
    const module = payloadModule();
    return this.queryCount(
      module._fdb_payload_v1_spec_component_count.bind(module),
    );
  }

  componentId(index: number): string {
    const module = payloadModule();
    return this.queryIndexedId(
      module._fdb_payload_v1_spec_component_id.bind(module),
      checkedU32(index, 'componentIndex'),
    );
  }

  componentIndex(identifier: string): number {
    const module = payloadModule();
    return this.queryNamedIndex(
      module._fdb_payload_v1_spec_component_index.bind(module),
      identifier,
    );
  }

  componentFieldCount(componentIndex: number): number {
    const module = payloadModule();
    return withAllocation(module, POINTER_SIZE * 2, (outputs) => {
      const status = module._fdb_payload_v1_spec_component_field_count(
        this.requireHandle(),
        checkedU32(componentIndex, 'componentIndex'),
        outputs,
        outputs + POINTER_SIZE,
      );
      checkStatus(status, outputs + POINTER_SIZE);
      return readU32(module, outputs);
    });
  }

  componentFieldId(componentIndex: number, fieldIndex: number): string {
    const module = payloadModule();
    const bytes = this.queryBlob((spec, outBlob, outError) =>
      module._fdb_payload_v1_spec_component_field_id(
        spec,
        checkedU32(componentIndex, 'componentIndex'),
        checkedU32(fieldIndex, 'fieldIndex'),
        outBlob,
        outError,
      ),
    );
    try {
      return decoder.decode(bytes);
    } catch (error) {
      throw bindingError(
        `FastDB Core returned a non-UTF-8 field identifier: ${String(error)}`,
        'invalid_utf8_identifier',
      );
    }
  }

  componentFieldIndex(componentIndex: number, identifier: string): number {
    const module = payloadModule();
    const bytes = encoder.encode(identifier);
    return withInputBytes(module, bytes, (data, size) =>
      withAllocation(module, POINTER_SIZE * 2, (outputs) => {
        const status = module._fdb_payload_v1_spec_component_field_index(
          this.requireHandle(),
          checkedU32(componentIndex, 'componentIndex'),
          data,
          size,
          outputs,
          outputs + POINTER_SIZE,
        );
        checkStatus(status, outputs + POINTER_SIZE);
        return readU32(module, outputs);
      }),
    );
  }

  private requireHandle(): number {
    if (this.handle === 0) {
      throw bindingError('CompiledSpec is disposed', 'disposed_handle');
    }
    return this.handle;
  }

  private queryBlob(query: BlobQuery): Uint8Array {
    const module = payloadModule();
    return withAllocation(module, POINTER_SIZE * 2, (outputs) => {
      const status = query(
        this.requireHandle(),
        outputs,
        outputs + POINTER_SIZE,
      );
      checkStatus(status, outputs + POINTER_SIZE);
      const blob = readU32(module, outputs);
      if (blob === 0) {
        throw bindingError(
          'FastDB Core returned success without a blob',
          'missing_blob_handle',
        );
      }
      try {
        return copyBytes(
          module,
          module._fdb_payload_v1_blob_data(blob),
          module._fdb_payload_v1_blob_size(blob),
        );
      } finally {
        module._fdb_payload_v1_blob_release(blob);
      }
    });
  }

  private queryCount(query: CountQuery): number {
    const module = payloadModule();
    return withAllocation(module, POINTER_SIZE * 2, (outputs) => {
      const status = query(
        this.requireHandle(),
        outputs,
        outputs + POINTER_SIZE,
      );
      checkStatus(status, outputs + POINTER_SIZE);
      return readU32(module, outputs);
    });
  }

  private queryIndexedId(query: IndexedIdQuery, index: number): string {
    const bytes = this.queryBlob((spec, outBlob, outError) =>
      query(spec, index, outBlob, outError),
    );
    try {
      return decoder.decode(bytes);
    } catch (error) {
      throw bindingError(
        `FastDB Core returned a non-UTF-8 identifier: ${String(error)}`,
        'invalid_utf8_identifier',
      );
    }
  }

  private queryNamedIndex(query: NamedIndexQuery, identifier: string): number {
    const module = payloadModule();
    const bytes = encoder.encode(identifier);
    return withInputBytes(module, bytes, (data, size) =>
      withAllocation(module, POINTER_SIZE * 2, (outputs) => {
        const status = query(
          this.requireHandle(),
          data,
          size,
          outputs,
          outputs + POINTER_SIZE,
        );
        checkStatus(status, outputs + POINTER_SIZE);
        return readU32(module, outputs);
      }),
    );
  }
}

function checkedProfile(value: number): Profile {
  if (value === Profile.RecordV1 || value === Profile.ObjectGraphV1) {
    return value;
  }
  throw new PayloadError(
    7002,
    'UNSUPPORTED_ABI',
    '/profile',
    'FastDB Core returned an unknown payload profile',
    `{"actual":${value},"reason":"unknown_profile"}`,
  );
}
