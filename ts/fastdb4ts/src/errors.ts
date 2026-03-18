export class FastdbError extends Error {
  constructor(message: string) {
    super(message);
    this.name = 'FastdbError';
  }
}

export class FastdbSchemaError extends FastdbError {
  constructor(message: string) {
    super(message);
    this.name = 'FastdbSchemaError';
  }
}

export class FastdbRuntimeError extends FastdbError {
  constructor(message: string) {
    super(message);
    this.name = 'FastdbRuntimeError';
  }
}

export class FastdbUsageError extends FastdbError {
  constructor(message: string) {
    super(message);
    this.name = 'FastdbUsageError';
  }
}
