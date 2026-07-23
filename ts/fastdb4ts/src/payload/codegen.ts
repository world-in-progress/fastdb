import {
  POINTER_SIZE,
  SHA256_SIZE,
  checkedU32,
  checkedU64,
  copyBytes,
  payloadModule,
  readU32,
  readU64,
  withAllocation,
  writeU32,
  writeU64,
} from './abi.js';
import { bindingError, checkStatus } from './error.js';

const CODEGEN_OPTIONS_SIZE = 48;

export enum CodegenTarget {
  Cpp = 1 << 0,
  Rust = 1 << 1,
  Python = 1 << 2,
  TypeScript = 1 << 3,
}

export enum ArtifactKind {
  Source = 1,
}

export interface CodegenOptionValues {
  flags?: number;
  maxArtifacts?: bigint;
  maxTotalBytes?: bigint;
}

export class CodegenOptions {
  readonly flags: number;
  readonly maxArtifacts: bigint;
  readonly maxTotalBytes: bigint;

  constructor(values: CodegenOptionValues = {}) {
    const module = payloadModule();
    const defaults = withAllocation(module, CODEGEN_OPTIONS_SIZE, (pointer) => {
      module._fdb_payload_v1_codegen_options_init(pointer);
      return {
        flags: readU32(module, pointer + 4),
        maxArtifacts: readU64(module, pointer + 8),
        maxTotalBytes: readU64(module, pointer + 16),
      };
    });
    this.flags = checkedU32(values.flags ?? defaults.flags, 'flags');
    this.maxArtifacts = checkedU64(
      values.maxArtifacts ?? defaults.maxArtifacts,
      'maxArtifacts',
    );
    this.maxTotalBytes = checkedU64(
      values.maxTotalBytes ?? defaults.maxTotalBytes,
      'maxTotalBytes',
    );
  }
}

export class Artifact {
  readonly relativePath: string;
  readonly kind: ArtifactKind;
  readonly #bytes: Uint8Array;
  readonly #sha256: Uint8Array;

  constructor(
    relativePath: string,
    kind: ArtifactKind,
    bytes: Uint8Array,
    sha256: Uint8Array,
  ) {
    this.relativePath = relativePath;
    this.kind = kind;
    this.#bytes = bytes.slice();
    this.#sha256 = sha256.slice();
  }

  get bytes(): Uint8Array {
    return this.#bytes.slice();
  }

  get sha256(): Uint8Array {
    return this.#sha256.slice();
  }
}

const decoder = new TextDecoder('utf-8', { fatal: true });
const artifactSetToken = Symbol('fastdb.payload.artifactSetToken');
const createArtifactSet = Symbol('fastdb.payload.createArtifactSet');

const artifactSetFinalizer = new FinalizationRegistry<number>((handle) => {
  try {
    payloadModule()._fdb_payload_v1_codegen_result_release(handle);
  } catch {
    // Explicit disposal is authoritative; finalization is only a fallback.
  }
});

type BlobQuery = (
  result: number,
  index: bigint,
  outBlob: number,
  outError: number,
) => number;

export class ArtifactSet {
  #handle: number;
  readonly #finalizerToken = {};

  private constructor(handle: number, token: typeof artifactSetToken) {
    if (token !== artifactSetToken) {
      throw new TypeError('ArtifactSet handles are created only by FastDB');
    }
    if (handle === 0) {
      throw bindingError(
        'FastDB Core returned success without an artifact set',
        'missing_codegen_result',
      );
    }
    this.#handle = handle;
    artifactSetFinalizer.register(this, handle, this.#finalizerToken);
  }

  static [createArtifactSet](
    handle: number,
    token: typeof artifactSetToken,
  ): ArtifactSet {
    if (token !== artifactSetToken) {
      throw new TypeError('ArtifactSet handles are created only by FastDB');
    }
    try {
      return new ArtifactSet(handle, artifactSetToken);
    } catch (error) {
      if (handle !== 0) {
        payloadModule()._fdb_payload_v1_codegen_result_release(handle);
      }
      throw error;
    }
  }

  clone(): ArtifactSet {
    const handle = this.requireHandle();
    payloadModule()._fdb_payload_v1_codegen_result_retain(handle);
    return ArtifactSet[createArtifactSet](handle, artifactSetToken);
  }

  dispose(): void {
    if (this.#handle !== 0) {
      const handle = this.#handle;
      this.#handle = 0;
      artifactSetFinalizer.unregister(this.#finalizerToken);
      payloadModule()._fdb_payload_v1_codegen_result_release(handle);
    }
  }

  size(): bigint {
    const module = payloadModule();
    return withAllocation(module, 8 + POINTER_SIZE, (outputs) => {
      const status = module._fdb_payload_v1_codegen_result_artifact_count(
        this.requireHandle(),
        outputs,
        outputs + 8,
      );
      checkStatus(status, outputs + 8);
      return readU64(module, outputs);
    });
  }

  artifact(index: bigint): Artifact {
    const checkedIndex = checkedU64(index, 'artifactIndex');
    const module = payloadModule();
    const pathBytes = this.queryBlob(
      checkedIndex,
      module._fdb_payload_v1_codegen_result_artifact_relative_path.bind(module),
    );
    let relativePath: string;
    try {
      relativePath = decoder.decode(pathBytes);
    } catch (error) {
      throw bindingError(
        `FastDB Core returned a non-UTF-8 artifact path: ${String(error)}`,
        'invalid_utf8_artifact_path',
      );
    }

    const kind = withAllocation(module, POINTER_SIZE * 2, (outputs) => {
      const status = module._fdb_payload_v1_codegen_result_artifact_kind(
        this.requireHandle(),
        checkedIndex,
        outputs,
        outputs + POINTER_SIZE,
      );
      checkStatus(status, outputs + POINTER_SIZE);
      const value = readU32(module, outputs);
      if (value !== ArtifactKind.Source) {
        throw bindingError(
          'FastDB Core returned an unknown artifact kind',
          'unknown_artifact_kind',
        );
      }
      return value as ArtifactKind;
    });
    const bytes = this.queryBlob(
      checkedIndex,
      module._fdb_payload_v1_codegen_result_artifact_bytes.bind(module),
    );
    const sha256 = withAllocation(
      module,
      SHA256_SIZE + POINTER_SIZE,
      (outputs) => {
        const status =
          module._fdb_payload_v1_codegen_result_artifact_sha256(
            this.requireHandle(),
            checkedIndex,
            outputs,
            outputs + SHA256_SIZE,
          );
        checkStatus(status, outputs + SHA256_SIZE);
        return module.HEAPU8.slice(outputs, outputs + SHA256_SIZE);
      },
    );
    return new Artifact(relativePath, kind, bytes, sha256);
  }

  private queryBlob(index: bigint, query: BlobQuery): Uint8Array {
    const module = payloadModule();
    return withAllocation(module, POINTER_SIZE * 2, (outputs) => {
      const status = query(
        this.requireHandle(),
        index,
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

  private requireHandle(): number {
    if (this.#handle === 0) {
      throw bindingError('ArtifactSet is disposed', 'disposed_handle');
    }
    return this.#handle;
  }
}

export function generateArtifacts(
  specHandle: number,
  target: CodegenTarget,
  options = new CodegenOptions(),
): ArtifactSet {
  if (
    target !== CodegenTarget.Cpp &&
    target !== CodegenTarget.Rust &&
    target !== CodegenTarget.Python &&
    target !== CodegenTarget.TypeScript
  ) {
    throw new RangeError('target must be a CodegenTarget');
  }
  const module = payloadModule();
  return withAllocation(module, CODEGEN_OPTIONS_SIZE, (rawOptions) => {
    module._fdb_payload_v1_codegen_options_init(rawOptions);
    writeU32(module, rawOptions + 4, checkedU32(options.flags, 'flags'));
    writeU64(
      module,
      rawOptions + 8,
      checkedU64(options.maxArtifacts, 'maxArtifacts'),
    );
    writeU64(
      module,
      rawOptions + 16,
      checkedU64(options.maxTotalBytes, 'maxTotalBytes'),
    );
    return withAllocation(module, POINTER_SIZE * 2, (outputs) => {
      const status = module._fdb_payload_v1_spec_codegen(
        specHandle,
        BigInt(target),
        rawOptions,
        outputs,
        outputs + POINTER_SIZE,
      );
      checkStatus(status, outputs + POINTER_SIZE);
      return ArtifactSet[createArtifactSet](
        readU32(module, outputs),
        artifactSetToken,
      );
    });
  });
}
