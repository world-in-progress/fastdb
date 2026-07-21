import {
  copyBytes,
  payloadModule,
  readU32,
  readU64,
  withAllocation,
} from './abi.js';

type ErrorFieldAccessor = (
  error: number,
  outData: number,
  outSize: number,
) => void;

const decoder = new TextDecoder('utf-8');
const ERROR_SIZE_OFFSET = 8;
const ERROR_OUTPUT_SIZE = 16;

export class PayloadError extends Error {
  readonly code: number;
  readonly symbol: string;
  readonly path: string;
  readonly detailsJson: string;

  constructor(
    code: number,
    symbol: string,
    path: string,
    message: string,
    detailsJson: string,
  ) {
    super(message);
    this.name = 'PayloadError';
    this.code = code;
    this.symbol = symbol;
    this.path = path;
    this.detailsJson = detailsJson;
  }
}

export function bindingError(message: string, reason: string): PayloadError {
  return new PayloadError(
    9001,
    'BINDING_CONTRACT',
    '',
    message,
    `{"reason":"${reason}"}`,
  );
}

function copyErrorField(error: number, accessor: ErrorFieldAccessor): string {
  const module = payloadModule();
  return withAllocation(module, ERROR_OUTPUT_SIZE, (outputs) => {
    accessor(error, outputs, outputs + ERROR_SIZE_OFFSET);
    const data = readU32(module, outputs);
    const size = readU64(module, outputs + ERROR_SIZE_OFFSET);
    return decoder.decode(copyBytes(module, data, size));
  });
}

function takeNativeError(status: number, error: number): PayloadError {
  if (error === 0) {
    return new PayloadError(
      status || 9001,
      'BINDING_CONTRACT',
      '',
      'FastDB Core returned a failure without an owned error',
      '{"reason":"missing_error_handle"}',
    );
  }
  const module = payloadModule();
  try {
    return new PayloadError(
      module._fdb_payload_v1_error_code(error),
      copyErrorField(error, module._fdb_payload_v1_error_symbol.bind(module)),
      copyErrorField(error, module._fdb_payload_v1_error_path.bind(module)),
      copyErrorField(error, module._fdb_payload_v1_error_message.bind(module)),
      copyErrorField(
        error,
        module._fdb_payload_v1_error_details_json.bind(module),
      ),
    );
  } finally {
    module._fdb_payload_v1_error_release(error);
  }
}

export function checkStatus(status: number, errorOut: number): void {
  const module = payloadModule();
  const error = readU32(module, errorOut);
  if (status !== 0) {
    throw takeNativeError(status, error);
  }
  if (error !== 0) {
    module._fdb_payload_v1_error_release(error);
    throw bindingError(
      'FastDB Core returned success with an error handle',
      'unexpected_error_handle',
    );
  }
}
