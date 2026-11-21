#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <new>

#include "fastdb_payload.h"

namespace {

bool python_available() {
#if PY_VERSION_HEX >= 0x030D0000
    return Py_IsInitialized() && !Py_IsFinalizing();
#else
    return Py_IsInitialized();
#endif
}

struct Buffer {
    Py_buffer view{};

    ~Buffer() { reset(); }

    void reset() { PyBuffer_Release(&view); }

    bool acquire(PyObject* source, int flags) {
        if (PyObject_GetBuffer(source, &view, flags) < 0) {
            return false;
        }
        if (!PyBuffer_IsContiguous(&view, 'C')) {
            reset();
            PyErr_SetString(PyExc_BufferError, "expected a C-contiguous buffer");
            return false;
        }
        return true;
    }
};

struct BufferOwner {
    std::atomic<std::size_t> references{1};
    Buffer buffer;
};

constexpr char buffer_capsule[] = "fastdb4py.payload.buffer";
constexpr char access_capsule[] = "fastdb4py.payload.access";

void release_buffer(void*, void* token) {
    auto* owner = static_cast<BufferOwner*>(token);
    if (owner->references.fetch_sub(1, std::memory_order_acq_rel) == 1 &&
        python_available()) {
        const auto state = PyGILState_Ensure();
        delete owner;
        PyGILState_Release(state);
    }
}

fdb_payload_v1_status_t retain_buffer(void*, void* token) {
    if (!python_available()) {
        return FDB_PAYLOAD_E_ALLOCATION_FAILED;
    }
    auto* owner = static_cast<BufferOwner*>(token);
    owner->references.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

void destroy_buffer_capsule(PyObject* capsule) {
    release_buffer(nullptr, PyCapsule_GetPointer(capsule, buffer_capsule));
}

PyObject* make_buffer_owner(PyObject* source, bool snapshot_writable) {
    auto* owner = new (std::nothrow) BufferOwner;
    if (owner == nullptr) {
        return PyErr_NoMemory();
    }
    if (!owner->buffer.acquire(source, PyBUF_SIMPLE)) {
        delete owner;
        return nullptr;
    }
    auto& view = owner->buffer.view;
    if (snapshot_writable && !view.readonly) {
        PyObject* snapshot = PyBytes_FromStringAndSize(
            static_cast<const char*>(view.buf), view.len);
        owner->buffer.reset();
        if (snapshot == nullptr) {
            delete owner;
            return nullptr;
        }
        const bool acquired = owner->buffer.acquire(snapshot, PyBUF_SIMPLE);
        Py_DECREF(snapshot);
        if (!acquired) {
            delete owner;
            return nullptr;
        }
    }
    PyObject* capsule = PyCapsule_New(owner, buffer_capsule, destroy_buffer_capsule);
    if (capsule == nullptr) {
        delete owner;
    }
    return capsule;
}

PyObject* input_buffer(PyObject*, PyObject* source) {
    PyObject* pin = make_buffer_owner(source, false);
    if (pin == nullptr) {
        return nullptr;
    }
    const auto* owner = static_cast<BufferOwner*>(
        PyCapsule_GetPointer(pin, buffer_capsule));
    return Py_BuildValue("NKn", pin,
        static_cast<unsigned long long>(
            reinterpret_cast<std::uintptr_t>(owner->buffer.view.buf)),
        owner->buffer.view.len);
}

PyObject* external_bytes(PyObject*, PyObject* source) {
    return make_buffer_owner(source, true);
}

PyObject* open_external(PyObject*, PyObject* args) {
    PyObject* capsule = nullptr;
    unsigned long long spec = 0, options = 0, payload = 0, error = 0;
    if (!PyArg_ParseTuple(args, "OKKKK", &capsule, &spec, &options, &payload,
                          &error)) {
        return nullptr;
    }
    auto* owner = static_cast<BufferOwner*>(PyCapsule_GetPointer(capsule, buffer_capsule));
    if (owner == nullptr) {
        return nullptr;
    }
    fdb_payload_v1_backing_v1_t backing;
    fdb_payload_v1_backing_init(&backing);
    backing.retain = retain_buffer;
    backing.release = release_buffer;
    const auto& view = owner->buffer.view;
    fdb_payload_v1_status_t status;
    Py_BEGIN_ALLOW_THREADS
    status = fdb_payload_v1_payload_open_external(
        reinterpret_cast<const fdb_payload_v1_spec_t*>(spec),
        static_cast<const std::uint8_t*>(view.buf), static_cast<std::uint64_t>(view.len),
        &backing, owner, reinterpret_cast<const fdb_payload_v1_open_options_t*>(options),
        reinterpret_cast<fdb_payload_v1_payload_t**>(payload),
        reinterpret_cast<fdb_payload_v1_error_t**>(error));
    Py_END_ALLOW_THREADS
    return PyLong_FromUnsignedLong(status);
}

void destroy_access_capsule(PyObject* capsule) {
    auto* access = static_cast<fdb_payload_v1_access_t*>(
        PyCapsule_GetPointer(capsule, access_capsule));
    fdb_payload_v1_access_release(access);
}

PyObject* own_access(PyObject*, PyObject* handle) {
    void* access = PyLong_AsVoidPtr(handle);
    if (access == nullptr && PyErr_Occurred()) {
        return nullptr;
    }
    return PyCapsule_New(access, access_capsule, destroy_access_capsule);
}

struct ExportState {
    std::mutex mutex;
    PyObject* pin;
    void* data;
    Py_ssize_t size;
    std::size_t exports = 0;
};

struct Exporter {
    PyObject_HEAD
    ExportState* state;
};

int get_buffer(PyObject* self, Py_buffer* view, int flags) {
    auto& state = *reinterpret_cast<Exporter*>(self)->state;
    const std::lock_guard<std::mutex> lock(state.mutex);
    if (state.pin == nullptr) {
        PyErr_SetString(PyExc_BufferError, "payload buffer has been released");
        return -1;
    }
    if (PyBuffer_FillInfo(view, self, state.data, state.size, 1, flags) < 0) {
        return -1;
    }
    ++state.exports;
    return 0;
}

void release_export(PyObject* self, Py_buffer*) {
    auto& state = *reinterpret_cast<Exporter*>(self)->state;
    PyObject* pin = nullptr;
    {
        const std::lock_guard<std::mutex> lock(state.mutex);
        if (--state.exports == 0) {
            pin = state.pin;
            state.pin = nullptr;
        }
    }
    Py_XDECREF(pin);
}

void destroy_exporter(PyObject* self) {
    auto* state = reinterpret_cast<Exporter*>(self)->state;
    if (state != nullptr) {
        Py_XDECREF(state->pin);
        delete state;
    }
    auto* type = Py_TYPE(self);
    type->tp_free(self);
    Py_DECREF(type);
}

struct ModuleState {
    PyTypeObject* exporter;
};

PyObject* export_memoryview(PyObject* module, PyObject* args) {
    PyObject* pin = nullptr;
    unsigned long long address = 0;
    Py_ssize_t size = 0;
    if (!PyArg_ParseTuple(args, "OKn", &pin, &address, &size)) {
        return nullptr;
    }
    if (!PyCapsule_IsValid(pin, access_capsule) || size < 0 ||
        (size > 0 && address == 0)) {
        PyErr_SetString(PyExc_ValueError, "invalid payload buffer span");
        return nullptr;
    }
    auto* module_state = static_cast<ModuleState*>(PyModule_GetState(module));
    auto* type = module_state->exporter;
    auto* exporter = reinterpret_cast<Exporter*>(type->tp_alloc(type, 0));
    if (exporter == nullptr) {
        return nullptr;
    }
    static char empty_span = 0;
    exporter->state = new (std::nothrow) ExportState{
        {}, pin, address == 0 ? &empty_span : reinterpret_cast<void*>(address), size};
    if (exporter->state == nullptr) {
        Py_DECREF(exporter);
        return PyErr_NoMemory();
    }
    Py_INCREF(pin);
    auto* object = reinterpret_cast<PyObject*>(exporter);
    PyObject* result = PyMemoryView_FromObject(object);
    Py_DECREF(object);
    return result;
}

struct Reservation {
    std::atomic<std::size_t> references{1};
    Buffer writable;
    Buffer readable;
    PyObject* callbacks = nullptr;
    PyObject* token = nullptr;

    ~Reservation() {
        unpin();
        Py_XDECREF(token);
        Py_XDECREF(callbacks);
    }

    void unpin() {
        readable.reset();
        writable.reset();
    }
};

enum Callback { reserve_index, commit_index, rollback_index, release_index };

bool aligned(const Py_buffer& view, std::uint32_t alignment) {
    return alignment != 0 &&
           reinterpret_cast<std::uintptr_t>(view.buf) % alignment == 0;
}

fdb_payload_v1_status_t reserve(
    void* context, std::uint32_t mode, std::uint64_t minimum_capacity,
    std::uint32_t alignment, void** out_token, std::uint8_t** out_data,
    std::uint64_t* out_capacity) {
    const auto gil = PyGILState_Ensure();
    auto* callbacks = static_cast<PyObject*>(context);
    auto* result = PyObject_CallFunction(PyTuple_GET_ITEM(callbacks, reserve_index),
        "IKI", mode, static_cast<unsigned long long>(minimum_capacity), alignment);
    auto* reservation = new (std::nothrow) Reservation;
    auto status = mode == FDB_PAYLOAD_RESERVE_DIRECT
        ? FDB_PAYLOAD_E_DIRECT_UNAVAILABLE : FDB_PAYLOAD_E_ALLOCATION_FAILED;
    if (reservation == nullptr || PyErr_ExceptionMatches(PyExc_MemoryError)) {
        status = FDB_PAYLOAD_E_ALLOCATION_FAILED;
    } else if (result != nullptr && result != Py_None && PyTuple_Check(result) &&
               PyTuple_GET_SIZE(result) == 2 &&
               reservation->writable.acquire(PyTuple_GET_ITEM(result, 0), PyBUF_WRITABLE)) {
        const auto& view = reservation->writable.view;
        if (!view.readonly && static_cast<std::uint64_t>(view.len) >= minimum_capacity &&
            aligned(view, alignment) && view.buf != nullptr) {
            reservation->callbacks = Py_NewRef(callbacks);
            reservation->token = Py_NewRef(PyTuple_GET_ITEM(result, 1));
            *out_token = reservation;
            *out_data = static_cast<std::uint8_t*>(view.buf);
            *out_capacity = static_cast<std::uint64_t>(view.len);
            reservation = nullptr;
            status = 0;
        }
    }
    delete reservation;
    Py_XDECREF(result);
    PyErr_Clear();
    PyGILState_Release(gil);
    return status;
}

fdb_payload_v1_status_t commit(
    void*, void* token, std::uint64_t used, const std::uint8_t** out_data,
    std::uint64_t* out_size) {
    const auto gil = PyGILState_Ensure();
    auto& reservation = *static_cast<Reservation*>(token);
    auto* callback = PyTuple_GET_ITEM(reservation.callbacks, commit_index);
    auto* result = callback == Py_None ? Py_NewRef(Py_None)
        : PyObject_CallFunction(callback, "OK", reservation.token,
                                static_cast<unsigned long long>(used));
    auto status = FDB_PAYLOAD_E_COMMIT_FAILED;
    if (result != nullptr) {
        const Py_buffer* view = &reservation.writable.view;
        if (result != Py_None) {
            if (reservation.readable.acquire(result, PyBUF_SIMPLE)) {
                reservation.writable.reset();
                view = &reservation.readable.view;
            } else {
                view = nullptr;
            }
        }
        if (view != nullptr && view->buf != nullptr &&
            static_cast<std::uint64_t>(view->len) >= used) {
            *out_data = static_cast<const std::uint8_t*>(view->buf);
            *out_size = used;
            status = 0;
        }
    }
    Py_XDECREF(result);
    PyErr_Clear();
    PyGILState_Release(gil);
    return status;
}

fdb_payload_v1_status_t rollback(void*, void* token) {
    const auto gil = PyGILState_Ensure();
    auto* reservation = static_cast<Reservation*>(token);
    reservation->unpin();
    auto* callback = PyTuple_GET_ITEM(reservation->callbacks, rollback_index);
    auto* result = callback == Py_None ? Py_NewRef(Py_None)
        : PyObject_CallOneArg(callback, reservation->token);
    const auto status = result == nullptr ? FDB_PAYLOAD_E_ROLLBACK_FAILED : 0;
    Py_XDECREF(result);
    delete reservation;
    PyErr_Clear();
    PyGILState_Release(gil);
    return status;
}

fdb_payload_v1_status_t retain_reservation(void*, void* token) {
    if (!python_available()) {
        return FDB_PAYLOAD_E_ALLOCATION_FAILED;
    }
    auto* reservation = static_cast<Reservation*>(token);
    reservation->references.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

void release_reservation(void*, void* token) {
    auto* reservation = static_cast<Reservation*>(token);
    if (reservation->references.fetch_sub(1, std::memory_order_acq_rel) != 1 ||
        !python_available()) {
        return;
    }
    const auto gil = PyGILState_Ensure();
    reservation->unpin();
    auto* callback = PyTuple_GET_ITEM(reservation->callbacks, release_index);
    if (callback != Py_None) {
        // A release may run during Python exception propagation.
#if PY_VERSION_HEX >= 0x030C0000
        PyObject* exc = PyErr_GetRaisedException();
        auto* result = PyObject_CallOneArg(callback, reservation->token);
        if (result == nullptr) {
            PyErr_WriteUnraisable(callback);
        }
        Py_XDECREF(result);
        PyErr_SetRaisedException(exc);
#else
        PyObject* type = nullptr;
        PyObject* value = nullptr;
        PyObject* traceback = nullptr;
        PyErr_Fetch(&type, &value, &traceback);
        auto* result = PyObject_CallOneArg(callback, reservation->token);
        if (result == nullptr) {
            PyErr_WriteUnraisable(callback);
        }
        Py_XDECREF(result);
        PyErr_Restore(type, value, traceback);
#endif
    }
    delete reservation;
    PyGILState_Release(gil);
}

PyObject* execute_backed(PyObject*, PyObject* args) {
    unsigned long long plan = 0, payload = 0, report = 0, error = 0;
    unsigned int policy = 0;
    PyObject* callbacks = nullptr;
    if (!PyArg_ParseTuple(args, "KIO!KKK", &plan, &policy, &PyTuple_Type,
                          &callbacks, &payload, &report, &error)) {
        return nullptr;
    }
    if (PyTuple_GET_SIZE(callbacks) != 4) {
        PyErr_SetString(PyExc_ValueError, "expected four backing callbacks");
        return nullptr;
    }
    fdb_payload_v1_backing_v1_t backing;
    fdb_payload_v1_backing_init(&backing);
    backing.context = callbacks;
    backing.reserve = reserve;
    backing.commit = commit;
    backing.rollback = rollback;
    backing.retain = retain_reservation;
    backing.release = release_reservation;
    fdb_payload_v1_status_t status;
    Py_BEGIN_ALLOW_THREADS
    status = fdb_payload_v1_plan_execute(
        reinterpret_cast<const fdb_payload_v1_plan_t*>(plan), policy, &backing,
        reinterpret_cast<fdb_payload_v1_payload_t**>(payload),
        reinterpret_cast<fdb_payload_v1_execution_report_t*>(report),
        reinterpret_cast<fdb_payload_v1_error_t**>(error));
    Py_END_ALLOW_THREADS
    return PyLong_FromUnsignedLong(status);
}

PyMethodDef methods[] = {
    {"input_buffer", input_buffer, METH_O, nullptr},
    {"external_bytes", external_bytes, METH_O, nullptr},
    {"open_external", open_external, METH_VARARGS, nullptr},
    {"own_access", own_access, METH_O, nullptr},
    {"export_memoryview", export_memoryview, METH_VARARGS, nullptr},
    {"execute_backed", execute_backed, METH_VARARGS, nullptr},
    {nullptr, nullptr, 0, nullptr},
};

int module_exec(PyObject* module) {
    PyType_Slot exporter_slots[] = {
        {Py_bf_getbuffer, reinterpret_cast<void*>(get_buffer)},
        {Py_bf_releasebuffer, reinterpret_cast<void*>(release_export)},
        {Py_tp_dealloc, reinterpret_cast<void*>(destroy_exporter)},
        {0, nullptr},
    };
    PyType_Spec spec = {
        "fastdb4py.core._payload_ext._ReadOnlyBuffer", sizeof(Exporter), 0,
        Py_TPFLAGS_DEFAULT | Py_TPFLAGS_DISALLOW_INSTANTIATION, exporter_slots,
    };
    auto* state = static_cast<ModuleState*>(PyModule_GetState(module));
    state->exporter = reinterpret_cast<PyTypeObject*>(PyType_FromSpec(&spec));
    return state->exporter == nullptr ? -1 : 0;
}

int module_traverse(PyObject* module, visitproc visit, void* arg) {
    auto* state = static_cast<ModuleState*>(PyModule_GetState(module));
    Py_VISIT(state->exporter);
    return 0;
}

int module_clear(PyObject* module) {
    auto* state = static_cast<ModuleState*>(PyModule_GetState(module));
    Py_CLEAR(state->exporter);
    return 0;
}

void module_free(void* module) {
    module_clear(static_cast<PyObject*>(module));
}

PyModuleDef_Slot slots[] = {
    {Py_mod_exec, reinterpret_cast<void*>(module_exec)},
#if PY_VERSION_HEX >= 0x030C0000
    {Py_mod_multiple_interpreters, Py_MOD_MULTIPLE_INTERPRETERS_NOT_SUPPORTED},
#endif
#ifdef Py_GIL_DISABLED
    {Py_mod_gil, Py_MOD_GIL_NOT_USED},
#endif
    {0, nullptr},
};

PyModuleDef definition = {
    PyModuleDef_HEAD_INIT, "_payload_ext", nullptr, sizeof(ModuleState),
    methods, slots, module_traverse, module_clear, module_free,
};

}  // namespace

PyMODINIT_FUNC PyInit__payload_ext() {
    return PyModuleDef_Init(&definition);
}
