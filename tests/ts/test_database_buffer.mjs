import assert from 'node:assert/strict';
import test from 'node:test';

import {
  allocateFastdbOwnedBytes,
  loadDatabaseFromBytes,
} from '../../ts/fastdb4ts/dist/index.js';


test('owned database loading frees Wasm memory when native load throws', () => {
  const frees = [];
  const heap = new Uint8Array(32);
  const fakeModule = {
    HEAPU8: heap,
    WxDatabase: {
      loadFromOwnedHeap(ptr, size) {
        assert.deepEqual(Array.from(heap.slice(ptr, ptr + size)), [1, 2, 3, 5]);
        throw new Error('native load failed before taking ownership');
      },
    },
    _free(ptr) {
      frees.push(ptr);
    },
    _malloc(size) {
      assert.equal(size, 4);
      return 8;
    },
  };

  assert.throws(
    () => loadDatabaseFromBytes(fakeModule, new Uint8Array([1, 2, 3, 5])),
    /native load failed/
  );
  assert.deepEqual(frees, [8]);
});

test('owned database loading consumes a populated Wasm buffer without another copy', () => {
  const frees = [];
  const heap = new Uint8Array(64);
  let mallocCalls = 0;
  const database = { delete() {} };
  const loadCalls = [];
  const fakeModule = {
    HEAPU8: heap,
    WxDatabase: {
      loadFromOwnedHeap(ptr, size) {
        loadCalls.push([ptr, size]);
        assert.deepEqual(Array.from(heap.slice(ptr, ptr + size)), [9, 8, 7, 6]);
        return database;
      },
    },
    _free(ptr) {
      frees.push(ptr);
    },
    _malloc(size) {
      mallocCalls += 1;
      assert.equal(size, 4);
      return 16;
    },
  };

  const owned = allocateFastdbOwnedBytes(fakeModule, 4);
  assert.equal(mallocCalls, 1);
  owned.view.set([9, 8, 7, 6]);

  fakeModule._malloc = () => {
    throw new Error('owned buffer load must not allocate a second Wasm buffer');
  };
  fakeModule.HEAPU8.set = () => {
    throw new Error('owned buffer load must not copy through HEAPU8.set');
  };

  const loaded = loadDatabaseFromBytes(fakeModule, owned);
  assert.equal(loaded, database);
  assert.deepEqual(loadCalls, [[16, 4]]);
  assert.deepEqual(frees, []);
  assert.throws(() => owned.view, /already been transferred/);
  owned.release();
  assert.deepEqual(frees, []);
});

test('owned database buffer release frees its allocation before transfer', () => {
  const frees = [];
  const fakeModule = {
    HEAPU8: new Uint8Array(32),
    WxDatabase: {
      loadFromOwnedHeap() {
        throw new Error('released buffer must not reach native load');
      },
    },
    _free(ptr) {
      frees.push(ptr);
    },
    _malloc(size) {
      assert.equal(size, 4);
      return 12;
    },
  };

  const owned = allocateFastdbOwnedBytes(fakeModule, 4);
  assert.equal(owned.byteLength, 4);
  assert.equal(owned.dataPtr, 12);
  owned.release();
  owned.release();
  assert.deepEqual(frees, [12]);
  assert.throws(() => owned.view, /already been transferred or released/);
});

test('owned database loading frees the caller buffer when native load returns null', () => {
  const frees = [];
  const fakeModule = {
    HEAPU8: new Uint8Array(32),
    WxDatabase: {
      loadFromOwnedHeap() {
        return null;
      },
    },
    _free(ptr) {
      frees.push(ptr);
    },
    _malloc(size) {
      assert.equal(size, 4);
      return 20;
    },
  };

  const owned = allocateFastdbOwnedBytes(fakeModule, 4);
  assert.throws(
    () => loadDatabaseFromBytes(fakeModule, owned),
    /Failed to load fastdb buffer/
  );
  assert.deepEqual(frees, [20]);
  owned.release();
  assert.deepEqual(frees, [20]);
});
