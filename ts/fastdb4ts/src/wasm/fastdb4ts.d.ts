declare const FastdbWasm: (
  moduleOverrides?: Record<string, unknown>
) => Promise<{
  WxMemoryStream: new () => {
    reset(): void;
    delete(): void;
  };
}>;

export default FastdbWasm;
