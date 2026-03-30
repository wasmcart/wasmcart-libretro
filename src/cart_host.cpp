// cart_host.cpp — Core wasmcart host logic using V8 WASM API (via libnode)
// Drop-in replacement for cart_host.c (wasmtime version).
// All public API (wasmcart_host.h) stays identical.

// Provide the snapshot stub that libnode.a references but doesn't include.
// Embedders don't use snapshots — return null.
namespace node {
class SnapshotBuilder {
public:
    static const void* GetEmbeddedSnapshotData();
};
const void* SnapshotBuilder::GetEmbeddedSnapshotData() { return nullptr; }
}

#include "node.h"
#include "v8.h"
#include "v8-wasm.h"
#include "uv.h"

extern "C" {
#include "cart_host.h"
}

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#ifdef _WIN32
#include <windows.h>
#endif

// ─── V8 state (module-level, single isolate) ──────────────────────────────

static std::unique_ptr<node::MultiIsolatePlatform> g_platform;
v8::Isolate* g_isolate = nullptr;
static node::Environment* g_env = nullptr;
std::unique_ptr<node::CommonEnvironmentSetup> g_setup;
static bool g_v8_initialized = false;

// Per-host V8 persistent handles
struct v8_host_state {
    v8::Global<v8::Object> instance;     // WebAssembly.Instance
    v8::Global<v8::Object> exports_obj;  // instance.exports
    v8::Global<v8::Function> fn_wc_get_info;
    v8::Global<v8::Function> fn_wc_init;
    v8::Global<v8::Function> fn_wc_render;
    v8::Global<v8::Function> fn_initialize;
    v8::Global<v8::Function> fn_malloc;
    v8::Global<v8::Object> memory_obj;   // WebAssembly.Memory
};

// ─── V8 helpers ──────────────────────────────────────────────────────────

static v8::Local<v8::String> v8str(const char* s) {
    return v8::String::NewFromUtf8(g_isolate, s).ToLocalChecked();
}

v8::Local<v8::Context> ctx() {
    return g_setup->context();
}

static void refresh_memory(wc_host_t* host) {
    auto state = (v8_host_state*)host->v8_state;
    auto mem = state->memory_obj.Get(g_isolate);
    // Use WasmMemoryObject::Buffer() for direct access
    auto wasm_mem = v8::WasmMemoryObject::Cast(*mem);
    auto ab = wasm_mem->Buffer();
    host->memory = (uint8_t*)ab->Data();
    host->memory_size = (uint32_t)ab->ByteLength();
}

// ─── V8 initialization (once per process) ──────────────────────────────

static int v8_init() {
    if (g_v8_initialized) return 0;

    // Enable WASM exnref (standard exception handling) — same as --experimental-wasm-exnref
    v8::V8::SetFlagsFromString("--experimental-wasm-exnref");
    // Optimize WASM↔JS transitions
    v8::V8::SetFlagsFromString("--turbo-inline-js-wasm-calls");
    v8::V8::SetFlagsFromString("--turbo-optimize-inlined-js-wasm-wrappers");
    v8::V8::SetFlagsFromString("--turboshaft-wasm-in-js-inlining");

    std::vector<std::string> args = {"wasmcart-run"};
    auto result = node::InitializeOncePerProcess(args, {
        node::ProcessInitializationFlags::kNoInitializeV8,
        node::ProcessInitializationFlags::kNoInitializeNodeV8Platform
    });
    for (const std::string& err : result->errors()) {
        fprintf(stderr, "wasmcart: v8 init error: %s\n", err.c_str());
    }
    if (result->early_return() != 0) return -1;

    g_platform = node::MultiIsolatePlatform::Create(4);
    v8::V8::InitializePlatform(g_platform.get());
    v8::V8::Initialize();

    std::vector<std::string> errors;
    std::vector<std::string> exec_args;
    g_setup = node::CommonEnvironmentSetup::Create(
        g_platform.get(), &errors, args, exec_args);
    if (!g_setup) {
        for (const std::string& err : errors)
            fprintf(stderr, "wasmcart: %s\n", err.c_str());
        return -1;
    }

    g_isolate = g_setup->isolate();
    g_env = g_setup->env();
    g_v8_initialized = true;

    // Run LoadEnvironment to initialize Node.js built-ins (needed for WASI etc)
    {
        v8::Locker locker(g_isolate);
        v8::Isolate::Scope isolate_scope(g_isolate);
        v8::HandleScope handle_scope(g_isolate);
        v8::Context::Scope context_scope(ctx());
        node::LoadEnvironment(g_env, "");
        uv_run(g_setup->event_loop(), UV_RUN_NOWAIT);
    }

    return 0;
}

// ─── Lifecycle ─────────────────────────────────────────────────────────

extern "C" wc_host_t* wc_host_create(void) {
    wc_host_t* host = (wc_host_t*)calloc(1, sizeof(wc_host_t));
    if (!host) return NULL;

    if (v8_init() != 0) {
        free(host);
        return NULL;
    }

    host->v8_state = new v8_host_state();
    return host;
}

extern "C" void wc_host_destroy(wc_host_t* host) {
    if (!host) return;
    wc_archive_close(host);
    if (host->v8_state) {
        delete (v8_host_state*)host->v8_state;
    }
    free(host);
}

// ─── Parse wc_info_t from WASM memory (unchanged from wasmtime) ─────────

static void parse_cart_info(wc_host_t* host, uint32_t info_ptr) {
    uint8_t* mem = host->memory;
    wc_cart_info_t* info = &host->info;

    info->version        = wc_read_u32(mem, info_ptr + WC_INFO_VERSION);
    info->width          = wc_read_u32(mem, info_ptr + WC_INFO_WIDTH);
    info->height         = wc_read_u32(mem, info_ptr + WC_INFO_HEIGHT);
    info->fb_ptr         = wc_read_u32(mem, info_ptr + WC_INFO_FB_PTR);
    info->audio_ptr      = wc_read_u32(mem, info_ptr + WC_INFO_AUDIO_PTR);
    info->audio_cap      = wc_read_u32(mem, info_ptr + WC_INFO_AUDIO_CAP);
    info->audio_write_ptr = wc_read_u32(mem, info_ptr + WC_INFO_AUDIO_WRITE);
    info->input_ptr      = wc_read_u32(mem, info_ptr + WC_INFO_INPUT_PTR);
    info->save_ptr       = wc_read_u32(mem, info_ptr + WC_INFO_SAVE_PTR);
    info->save_size      = wc_read_u32(mem, info_ptr + WC_INFO_SAVE_SIZE);
    info->time_ptr       = wc_read_u32(mem, info_ptr + WC_INFO_TIME_PTR);
    info->host_info_ptr  = wc_read_u32(mem, info_ptr + WC_INFO_HOST_INFO_PTR);

    info->flags            = wc_read_u32(mem, info_ptr + WC_INFO_FLAGS);
    info->audio_sample_rate = wc_read_u32(mem, info_ptr + WC_INFO_AUDIO_RATE);
    info->pointer_ptr      = wc_read_u32(mem, info_ptr + WC_INFO_POINTER_PTR);
    info->keys_ptr         = wc_read_u32(mem, info_ptr + WC_INFO_KEYS_PTR);
    info->gpu_api          = wc_read_u32(mem, info_ptr + WC_INFO_GPU_API);

    // Determine rendering mode: gpu_api is authoritative, fall back to fb_ptr for old carts
    if (info->gpu_api > 0) {
        host->uses_gl = true;
    } else {
        host->uses_gl = (info->fb_ptr == 0);
    }
}

static void write_host_info(wc_host_t* host, const wc_host_options_t* opts) {
    uint32_t ptr = host->info.host_info_ptr;
    if (ptr == 0) return;
    refresh_memory(host);
    uint8_t* mem = host->memory;
    wc_write_u32(mem, ptr + WC_HOST_INFO_PREFERRED_WIDTH, opts ? opts->preferred_width : 0);
    wc_write_u32(mem, ptr + WC_HOST_INFO_PREFERRED_HEIGHT, opts ? opts->preferred_height : 0);
    wc_write_u32(mem, ptr + WC_HOST_INFO_HOST_FPS, opts ? opts->host_fps : 60);
    wc_write_u32(mem, ptr + WC_HOST_INFO_AUDIO_SAMPLE_RATE, opts ? opts->audio_sample_rate : 48000);
    wc_write_u32(mem, ptr + WC_HOST_INFO_FLAGS, 0);
}

// ─── Build WASM import object ──────────────────────────────────────────

// Helper: create a V8 function from a C callback that takes wc_host_t as data
typedef void (*host_fn_cb)(const v8::FunctionCallbackInfo<v8::Value>&);

static v8::Local<v8::Function> make_fn(host_fn_cb cb) {
    return v8::Function::New(ctx(), cb).ToLocalChecked();
}

// ─── Host function callbacks (env module) ─────────────────────────────

static wc_host_t* _current_host = nullptr; // set before instantiation

static void v8_wc_log(const v8::FunctionCallbackInfo<v8::Value>& args) {
    refresh_memory(_current_host);
    uint32_t ptr = args[0]->Uint32Value(ctx()).FromJust();
    uint32_t len = args[1]->Uint32Value(ctx()).FromJust();
    fprintf(stderr, "wasmcart [cart]: %.*s\n", (int)len, (const char*)(_current_host->memory + ptr));
}

static void v8_emscripten_notify_memory_growth(const v8::FunctionCallbackInfo<v8::Value>& args) {
    refresh_memory(_current_host);
}

static void v8_emscripten_memcpy_js(const v8::FunctionCallbackInfo<v8::Value>& args) {
    refresh_memory(_current_host);
    uint32_t dest = args[0]->Uint32Value(ctx()).FromJust();
    uint32_t src = args[1]->Uint32Value(ctx()).FromJust();
    uint32_t n = args[2]->Uint32Value(ctx()).FromJust();
    memmove(_current_host->memory + dest, _current_host->memory + src, n);
}

static void v8_wc_asset_size(const v8::FunctionCallbackInfo<v8::Value>& args) {
    refresh_memory(_current_host);
    uint32_t path_ptr = args[0]->Uint32Value(ctx()).FromJust();
    uint32_t path_len = args[1]->Uint32Value(ctx()).FromJust();
    char path[512];
    uint32_t len = path_len < 511 ? path_len : 511;
    memcpy(path, _current_host->memory + path_ptr, len);
    path[len] = '\0';
    int32_t size = wc_archive_asset_size(_current_host, path);
    args.GetReturnValue().Set(v8::Integer::New(g_isolate, size));
}

static void v8_wc_load_asset(const v8::FunctionCallbackInfo<v8::Value>& args) {
    refresh_memory(_current_host);
    uint32_t path_ptr = args[0]->Uint32Value(ctx()).FromJust();
    uint32_t path_len = args[1]->Uint32Value(ctx()).FromJust();
    uint32_t dest_ptr = args[2]->Uint32Value(ctx()).FromJust();
    uint32_t max_size = args[3]->Uint32Value(ctx()).FromJust();
    char path[512];
    uint32_t len = path_len < 511 ? path_len : 511;
    memcpy(path, _current_host->memory + path_ptr, len);
    path[len] = '\0';
    if (dest_ptr + max_size > _current_host->memory_size) {
        args.GetReturnValue().Set(-1);
        return;
    }
    int32_t bytes = wc_archive_load_asset(_current_host, path,
        _current_host->memory + dest_ptr, max_size);
    args.GetReturnValue().Set(v8::Integer::New(g_isolate, bytes));
}

static void v8_noop(const v8::FunctionCallbackInfo<v8::Value>& args) {
    // no-op stub
}

static void v8_noop_return_0(const v8::FunctionCallbackInfo<v8::Value>& args) {
    args.GetReturnValue().Set(0);
}


// Build the env import object
static v8::Local<v8::Object> build_env_imports() {
    auto env = v8::Object::New(g_isolate);
    env->Set(ctx(), v8str("wc_log"), make_fn(v8_wc_log)).Check();
    env->Set(ctx(), v8str("wc_asset_size"), make_fn(v8_wc_asset_size)).Check();
    env->Set(ctx(), v8str("wc_load_asset"), make_fn(v8_wc_load_asset)).Check();
    env->Set(ctx(), v8str("emscripten_notify_memory_growth"), make_fn(v8_emscripten_notify_memory_growth)).Check();
    env->Set(ctx(), v8str("emscripten_memcpy_js"), make_fn(v8_emscripten_memcpy_js)).Check();
    env->Set(ctx(), v8str("emscripten_stack_init"), make_fn(v8_noop)).Check();
    env->Set(ctx(), v8str("__cxa_atexit"), make_fn(v8_noop_return_0)).Check();
    return env;
}

// ─── GL import callbacks will be registered by gl_imports.c ──────────
// gl_imports.c calls wc_gl_imports_init which uses the wasmtime linker.
// For V8, we need a different registration path. For now, we'll build
// the gl import object here and have gl_imports provide the callbacks.

// Helpers for gl_imports.cpp to access V8 state
extern "C" v8::Global<v8::Function>* wc_get_malloc_fn(wc_host_t* host) {
    auto state = (v8_host_state*)host->v8_state;
    return &state->fn_malloc;
}

extern "C" void wc_refresh_memory(wc_host_t* host) {
    refresh_memory(host);
}

// Forward declaration — gl_imports.cpp provides this
extern "C" void wc_gl_build_v8_imports(v8::Isolate* isolate, v8::Local<v8::Context> context,
    v8::Local<v8::Object> gl_obj, v8::Local<v8::Object> env_obj, wc_host_t* host);

// ─── WASI stub imports ──────────────────────────────────────────────────

static void v8_fd_write(const v8::FunctionCallbackInfo<v8::Value>& args) {
    // fd_write(fd, iovs_ptr, iovs_len, nwritten_ptr) -> errno
    refresh_memory(_current_host);
    uint32_t fd = args[0]->Uint32Value(ctx()).FromJust();
    uint32_t iovs_ptr = args[1]->Uint32Value(ctx()).FromJust();
    uint32_t iovs_len = args[2]->Uint32Value(ctx()).FromJust();
    uint32_t nwritten_ptr = args[3]->Uint32Value(ctx()).FromJust();

    uint8_t* mem = _current_host->memory;
    uint32_t total = 0;
    for (uint32_t i = 0; i < iovs_len; i++) {
        uint32_t buf_ptr = *(uint32_t*)(mem + iovs_ptr + i * 8);
        uint32_t buf_len = *(uint32_t*)(mem + iovs_ptr + i * 8 + 4);
        if (fd == 1) fwrite(mem + buf_ptr, 1, buf_len, stdout);
        else if (fd == 2) fwrite(mem + buf_ptr, 1, buf_len, stderr);
        total += buf_len;
    }
    *(uint32_t*)(mem + nwritten_ptr) = total;
    args.GetReturnValue().Set(0); // success
}

static void v8_fd_close(const v8::FunctionCallbackInfo<v8::Value>& args) {
    args.GetReturnValue().Set(0);
}

static void v8_fd_seek(const v8::FunctionCallbackInfo<v8::Value>& args) {
    args.GetReturnValue().Set(0);
}

static void v8_proc_exit(const v8::FunctionCallbackInfo<v8::Value>& args) {
    // Do nothing — cart tried to exit
}

static void v8_clock_time_get(const v8::FunctionCallbackInfo<v8::Value>& args) {
    // clock_time_get(id, precision, timestamp_ptr) -> errno
    refresh_memory(_current_host);
    uint32_t ts_ptr = args[2]->Uint32Value(ctx()).FromJust();
    uint64_t nanos;
#ifdef _WIN32
    LARGE_INTEGER freq, count;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    nanos = (uint64_t)((double)count.QuadPart / freq.QuadPart * 1000000000.0);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    nanos = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
#endif
    *(uint64_t*)(_current_host->memory + ts_ptr) = nanos;
    args.GetReturnValue().Set(0);
}

static void v8_random_get(const v8::FunctionCallbackInfo<v8::Value>& args) {
    refresh_memory(_current_host);
    uint32_t buf_ptr = args[0]->Uint32Value(ctx()).FromJust();
    uint32_t buf_len = args[1]->Uint32Value(ctx()).FromJust();
    uint8_t* dest = _current_host->memory + buf_ptr;
    for (uint32_t i = 0; i < buf_len; i++) dest[i] = rand() & 0xff;
    args.GetReturnValue().Set(0);
}

static v8::Local<v8::Object> build_wasi_imports() {
    auto wasi = v8::Object::New(g_isolate);
    wasi->Set(ctx(), v8str("fd_write"), make_fn(v8_fd_write)).Check();
    wasi->Set(ctx(), v8str("fd_close"), make_fn(v8_fd_close)).Check();
    wasi->Set(ctx(), v8str("fd_seek"), make_fn(v8_fd_seek)).Check();
    wasi->Set(ctx(), v8str("proc_exit"), make_fn(v8_proc_exit)).Check();
    wasi->Set(ctx(), v8str("clock_time_get"), make_fn(v8_clock_time_get)).Check();
    wasi->Set(ctx(), v8str("random_get"), make_fn(v8_random_get)).Check();
    // fd_read, fd_prestat_get, fd_prestat_dir_name, environ_get, etc. — return error
    wasi->Set(ctx(), v8str("fd_read"), make_fn(v8_noop_return_0)).Check();
    wasi->Set(ctx(), v8str("fd_prestat_get"), make_fn(v8_noop_return_0)).Check();
    wasi->Set(ctx(), v8str("fd_prestat_dir_name"), make_fn(v8_noop_return_0)).Check();
    wasi->Set(ctx(), v8str("environ_get"), make_fn(v8_noop_return_0)).Check();
    wasi->Set(ctx(), v8str("environ_sizes_get"), make_fn(v8_noop_return_0)).Check();
    wasi->Set(ctx(), v8str("args_get"), make_fn(v8_noop_return_0)).Check();
    wasi->Set(ctx(), v8str("args_sizes_get"), make_fn(v8_noop_return_0)).Check();
    return wasi;
}

// ─── Load cart ─────────────────────────────────────────────────────────

extern "C" int wc_host_load_file(wc_host_t* host, const char* wasc_path, const wc_host_options_t* opts) {
    v8::Locker locker(g_isolate);
    v8::Isolate::Scope isolate_scope(g_isolate);
    v8::HandleScope handle_scope(g_isolate);
    v8::Context::Scope context_scope(ctx());

    _current_host = host;
    auto state = (v8_host_state*)host->v8_state;

    // 1. Open archive and read wasm bytes
    int rc = wc_archive_open(host, wasc_path);
    if (rc != 0) {
        fprintf(stderr, "wasmcart: failed to open %s\n", wasc_path);
        return rc;
    }

    // 2. Compile WASM module — V8 Liftoff baseline is near-instant
    v8::TryCatch try_catch(g_isolate);

    // Compile WASM module — V8 Liftoff baseline compiles instantly
    v8::MemorySpan<const uint8_t> wire_bytes(host->wasm_bytes, host->wasm_bytes_len);
    auto maybe_module = v8::WasmModuleObject::Compile(g_isolate, wire_bytes);
    if (maybe_module.IsEmpty()) {
        if (try_catch.HasCaught()) {
            v8::String::Utf8Value err(g_isolate, try_catch.Exception());
            fprintf(stderr, "wasmcart: compile error: %s\n", *err);
        }
        return -1;
    }
    auto wasm_module = maybe_module.ToLocalChecked();

    // Need WebAssembly.Instance constructor for instantiation (V8 has no C++ instantiate API)
    auto global = ctx()->Global();
    auto wasm_ns = global->Get(ctx(), v8str("WebAssembly")).ToLocalChecked().As<v8::Object>();
    auto wasm_instance_ctor = wasm_ns->Get(ctx(), v8str("Instance")).ToLocalChecked().As<v8::Function>();

    // 3. Build import object
    auto imports = v8::Object::New(g_isolate);
    auto env_imports = build_env_imports();
    auto wasi_imports = build_wasi_imports();

    imports->Set(ctx(), v8str("env"), env_imports).Check();
    imports->Set(ctx(), v8str("wasi_snapshot_preview1"), wasi_imports).Check();
    imports->Set(ctx(), v8str("wasi_unstable"), wasi_imports).Check();

    // GL imports
    auto gl_imports_obj = v8::Object::New(g_isolate);
    wc_gl_build_v8_imports(g_isolate, ctx(), gl_imports_obj, env_imports, host);
    imports->Set(ctx(), v8str("gl"), gl_imports_obj).Check();

    // 4. Auto-stub missing imports
    // V8 doesn't have wasmtime's "define_unknown_imports_as_default_values" — we must provide every import.
    // Use WebAssembly.Module.imports(module) to find what's needed, stub anything missing.
    {
        auto wasm_module_ns = wasm_ns->Get(ctx(), v8str("Module")).ToLocalChecked().As<v8::Function>();
        auto imports_fn = wasm_module_ns->Get(ctx(), v8str("imports")).ToLocalChecked().As<v8::Function>();
        v8::Local<v8::Value> imp_args[] = { wasm_module };
        auto imp_arr = imports_fn->Call(ctx(), wasm_ns, 1, imp_args).ToLocalChecked().As<v8::Array>();

        for (uint32_t i = 0; i < imp_arr->Length(); i++) {
            auto entry = imp_arr->Get(ctx(), i).ToLocalChecked().As<v8::Object>();
            v8::String::Utf8Value mod(g_isolate, entry->Get(ctx(), v8str("module")).ToLocalChecked());
            v8::String::Utf8Value name(g_isolate, entry->Get(ctx(), v8str("name")).ToLocalChecked());
            v8::String::Utf8Value kind(g_isolate, entry->Get(ctx(), v8str("kind")).ToLocalChecked());

            // Ensure module namespace exists in imports
            auto mod_str = v8str(*mod);
            auto mod_val = imports->Get(ctx(), mod_str).ToLocalChecked();
            v8::Local<v8::Object> mod_obj;
            if (mod_val->IsObject()) {
                mod_obj = mod_val.As<v8::Object>();
            } else {
                mod_obj = v8::Object::New(g_isolate);
                imports->Set(ctx(), mod_str, mod_obj).Check();
            }

            // Check if already provided
            auto name_str = v8str(*name);
            auto existing = mod_obj->Get(ctx(), name_str).ToLocalChecked();
            if (!existing->IsUndefined()) continue;

            // Auto-stub based on kind
            if (strcmp(*kind, "function") == 0) {
                mod_obj->Set(ctx(), name_str, make_fn(v8_noop_return_0)).Check();
            } else if (strcmp(*kind, "global") == 0) {
                // Create a WebAssembly.Global with value 0
                auto global_ctor = wasm_ns->Get(ctx(), v8str("Global")).ToLocalChecked().As<v8::Function>();
                auto desc = v8::Object::New(g_isolate);
                desc->Set(ctx(), v8str("value"), v8str("i32")).Check();
                desc->Set(ctx(), v8str("mutable"), v8::Boolean::New(g_isolate, true)).Check();
                v8::Local<v8::Value> g_args[] = { desc, v8::Integer::New(g_isolate, 0) };
                auto wasm_global = global_ctor->NewInstance(ctx(), 2, g_args).ToLocalChecked();
                mod_obj->Set(ctx(), name_str, wasm_global).Check();
            } else if (strcmp(*kind, "table") == 0) {
                auto table_ctor = wasm_ns->Get(ctx(), v8str("Table")).ToLocalChecked().As<v8::Function>();
                auto desc = v8::Object::New(g_isolate);
                desc->Set(ctx(), v8str("element"), v8str("anyfunc")).Check();
                desc->Set(ctx(), v8str("initial"), v8::Integer::New(g_isolate, 0)).Check();
                v8::Local<v8::Value> t_args[] = { desc };
                auto wasm_table = table_ctor->NewInstance(ctx(), 1, t_args).ToLocalChecked();
                mod_obj->Set(ctx(), name_str, wasm_table).Check();
            } else if (strcmp(*kind, "memory") == 0) {
                auto mem_ctor = wasm_ns->Get(ctx(), v8str("Memory")).ToLocalChecked().As<v8::Function>();
                auto desc = v8::Object::New(g_isolate);
                desc->Set(ctx(), v8str("initial"), v8::Integer::New(g_isolate, 1)).Check();
                v8::Local<v8::Value> m_args[] = { desc };
                auto wasm_mem = mem_ctor->NewInstance(ctx(), 1, m_args).ToLocalChecked();
                mod_obj->Set(ctx(), name_str, wasm_mem).Check();
            }
        }
    }

    // 5. Instantiate: new WebAssembly.Instance(module, imports)
    v8::Local<v8::Value> instance_args[] = { wasm_module, imports };
    auto instance_result = wasm_instance_ctor->NewInstance(ctx(), 2, instance_args);
    if (instance_result.IsEmpty()) {
        if (try_catch.HasCaught()) {
            v8::String::Utf8Value err(g_isolate, try_catch.Exception());
            fprintf(stderr, "wasmcart: instantiate error: %s\n", *err);
        }
        return -1;
    }
    auto instance = instance_result.ToLocalChecked();
    state->instance.Reset(g_isolate, instance);

    // 5. Get exports
    auto exports = instance->Get(ctx(), v8str("exports")).ToLocalChecked().As<v8::Object>();
    state->exports_obj.Reset(g_isolate, exports);

    // Memory
    auto mem_val = exports->Get(ctx(), v8str("memory")).ToLocalChecked();
    if (!mem_val->IsUndefined()) {
        state->memory_obj.Reset(g_isolate, mem_val.As<v8::Object>());
        refresh_memory(host);
    }

    // Functions
    auto get_fn = [&](const char* name) -> v8::Local<v8::Function> {
        auto val = exports->Get(ctx(), v8str(name)).ToLocalChecked();
        if (val->IsFunction()) return val.As<v8::Function>();
        return v8::Local<v8::Function>();
    };

    auto fn_get_info = get_fn("wc_get_info");
    auto fn_init = get_fn("wc_init");
    auto fn_render = get_fn("wc_render");
    auto fn_initialize = get_fn("_initialize");
    auto fn_malloc = get_fn("malloc");

    if (fn_get_info.IsEmpty() || fn_render.IsEmpty()) {
        fprintf(stderr, "wasmcart: missing required exports (wc_get_info, wc_render)\n");
        return -1;
    }

    state->fn_wc_get_info.Reset(g_isolate, fn_get_info);
    state->fn_wc_render.Reset(g_isolate, fn_render);
    if (!fn_init.IsEmpty()) state->fn_wc_init.Reset(g_isolate, fn_init);
    if (!fn_initialize.IsEmpty()) state->fn_initialize.Reset(g_isolate, fn_initialize);
    if (!fn_malloc.IsEmpty()) state->fn_malloc.Reset(g_isolate, fn_malloc);

    // Store fn pointers for gl_imports to use
    host->fn_wc_get_info = &state->fn_wc_get_info;
    host->fn_wc_init = fn_init.IsEmpty() ? nullptr : &state->fn_wc_init;
    host->fn_wc_render = &state->fn_wc_render;

    // Pre-scan: detect GL usage from imports (needed before init for libretro HW render setup)
    // Check if any import is from "gl" module
    {
        auto wasm_module_ns = wasm_ns->Get(ctx(), v8str("Module")).ToLocalChecked().As<v8::Function>();
        auto imports_fn = wasm_module_ns->Get(ctx(), v8str("imports")).ToLocalChecked().As<v8::Function>();
        v8::Local<v8::Value> imp_args[] = { wasm_module };
        auto imp_arr = imports_fn->Call(ctx(), wasm_ns, 1, imp_args).ToLocalChecked().As<v8::Array>();
        for (uint32_t i = 0; i < imp_arr->Length(); i++) {
            auto entry = imp_arr->Get(ctx(), i).ToLocalChecked().As<v8::Object>();
            v8::String::Utf8Value mod(g_isolate, entry->Get(ctx(), v8str("module")).ToLocalChecked());
            v8::String::Utf8Value name(g_isolate, entry->Get(ctx(), v8str("name")).ToLocalChecked());
            v8::String::Utf8Value kind(g_isolate, entry->Get(ctx(), v8str("kind")).ToLocalChecked());
            // Pre-scan for GL imports (needed before wc_get_info for deferred init)
            // This is a fallback — gpu_api field in wc_info_t is authoritative after init
            if (strcmp(*mod, "gl") == 0 ||
                (strcmp(*mod, "env") == 0 && strcmp(*kind, "function") == 0 &&
                 (*name)[0] == 'g' && (*name)[1] == 'l' && (*name)[2] >= 'A' && (*name)[2] <= 'Z')) {
                host->uses_gl = true;
                break;
            }
        }
    }

    // 6-10. Init sequence (_initialize, wc_get_info, wc_init)
    // Can be deferred for GL carts that need the GL context first (libretro)
    if (opts && opts->defer_init) {
        host->init_deferred = true;
        host->info.width = (opts->preferred_width > 0) ? opts->preferred_width : 640;
        host->info.height = (opts->preferred_height > 0) ? opts->preferred_height : 480;
        // Store opts for write_host_info during finish_init
        if (opts) host->deferred_opts = *opts;
    } else {
        // Normal path: run full init now
        if (!fn_initialize.IsEmpty()) {
            fprintf(stderr, "wasmcart: calling _initialize\n");
            auto result = fn_initialize->Call(ctx(), ctx()->Global(), 0, nullptr);
            if (result.IsEmpty() && try_catch.HasCaught()) {
                v8::String::Utf8Value err(g_isolate, try_catch.Exception());
                fprintf(stderr, "wasmcart: _initialize error: %s\n", *err);
            }
            refresh_memory(host);
        }

        {
            auto result = fn_get_info->Call(ctx(), ctx()->Global(), 0, nullptr);
            if (!result.IsEmpty()) {
                refresh_memory(host);
                uint32_t info_ptr = result.ToLocalChecked()->Uint32Value(ctx()).FromJust();
                parse_cart_info(host, info_ptr);
            }
        }

        write_host_info(host, opts);

        if (opts && opts->save_data && host->info.save_ptr && opts->save_data_size > 0) {
            uint32_t copy_size = opts->save_data_size < host->info.save_size ?
                opts->save_data_size : host->info.save_size;
            memcpy(host->memory + host->info.save_ptr, opts->save_data, copy_size);
        }

        if (!fn_init.IsEmpty()) {
            auto result = fn_init->Call(ctx(), ctx()->Global(), 0, nullptr);
            if (result.IsEmpty() && try_catch.HasCaught()) {
                v8::String::Utf8Value err(g_isolate, try_catch.Exception());
                fprintf(stderr, "wasmcart: wc_init error: %s\n", *err);
                auto stack = try_catch.StackTrace(ctx());
                if (!stack.IsEmpty()) {
                    v8::String::Utf8Value st(g_isolate, stack.ToLocalChecked());
                    fprintf(stderr, "wasmcart: stack: %s\n", *st);
                }
            }
            refresh_memory(host);
        }

        // Re-read cart info
        {
            auto result = fn_get_info->Call(ctx(), ctx()->Global(), 0, nullptr);
            if (!result.IsEmpty()) {
                refresh_memory(host);
                parse_cart_info(host, result.ToLocalChecked()->Uint32Value(ctx()).FromJust());
            }
        }
    } // end else (non-deferred init)

    fprintf(stderr, "wasmcart: loaded %s (%ux%u, %s, ABI v%u)\n",
        host->manifest.name,
        host->info.width, host->info.height,
        host->uses_gl ? "GL" : "2D",
        host->info.version);

    return 0;
}

extern "C" int wc_host_load_memory(wc_host_t* host, const uint8_t* data, size_t len, const wc_host_options_t* opts) {
    int rc = wc_archive_open_memory(host, data, len);
    if (rc != 0) return rc;
    // TODO: shared init
    return -1;
}

// ─── Deferred init (for libretro GL carts — call after GL context ready) ──

extern "C" int wc_host_finish_init(wc_host_t* host) {
    if (!host || !host->init_deferred) return 0;

    v8::Locker locker(g_isolate);
    v8::Isolate::Scope isolate_scope(g_isolate);
    v8::HandleScope handle_scope(g_isolate);
    v8::Context::Scope context_scope(ctx());

    _current_host = host;
    auto state = (v8_host_state*)host->v8_state;
    v8::TryCatch try_catch(g_isolate);

    // Call _initialize
    if (!state->fn_initialize.IsEmpty()) {
        fprintf(stderr, "wasmcart: calling _initialize (deferred)\n");
        auto result = state->fn_initialize.Get(g_isolate)->Call(ctx(), ctx()->Global(), 0, nullptr);
        if (result.IsEmpty() && try_catch.HasCaught()) {
            v8::String::Utf8Value err(g_isolate, try_catch.Exception());
            fprintf(stderr, "wasmcart: _initialize error: %s\n", *err);
            return -1;
        }
        refresh_memory(host);
    }

    // Call wc_get_info
    {
        auto result = state->fn_wc_get_info.Get(g_isolate)->Call(ctx(), ctx()->Global(), 0, nullptr);
        if (!result.IsEmpty()) {
            refresh_memory(host);
            parse_cart_info(host, result.ToLocalChecked()->Uint32Value(ctx()).FromJust());
        }
    }

    // Write host info (preferred dimensions, etc.) before wc_init
    write_host_info(host, &host->deferred_opts);

    // Call wc_init
    if (!state->fn_wc_init.IsEmpty()) {
        auto result = state->fn_wc_init.Get(g_isolate)->Call(ctx(), ctx()->Global(), 0, nullptr);
        if (result.IsEmpty() && try_catch.HasCaught()) {
            v8::String::Utf8Value err(g_isolate, try_catch.Exception());
            fprintf(stderr, "wasmcart: wc_init error: %s\n", *err);
            return -1;
        }
        refresh_memory(host);
    }

    // Re-read info
    {
        auto result = state->fn_wc_get_info.Get(g_isolate)->Call(ctx(), ctx()->Global(), 0, nullptr);
        if (!result.IsEmpty()) {
            refresh_memory(host);
            parse_cart_info(host, result.ToLocalChecked()->Uint32Value(ctx()).FromJust());
        }
    }

    host->init_deferred = false;

    fprintf(stderr, "wasmcart: deferred init complete (%ux%u, %s)\n",
        host->info.width, host->info.height,
        host->uses_gl ? "GL" : "2D");

    return 0;
}

// ─── Input (unchanged — just writes to host->memory) ──────────────────

extern "C" void wc_host_set_pads(wc_host_t* host, const wc_pad_t pads[WC_MAX_PADS]) {
    if (!host->memory || !host->info.input_ptr) return;
    memcpy(host->memory + host->info.input_ptr, pads, sizeof(wc_pad_t) * WC_MAX_PADS);
}

extern "C" void wc_host_set_keyboard(wc_host_t* host, const uint8_t keys[WC_KEYS_STATE_SIZE]) {
    if (!host->memory || !host->info.keys_ptr) return;
    memcpy(host->memory + host->info.keys_ptr, keys, WC_KEYS_STATE_SIZE);
}

extern "C" void wc_host_set_pointer(wc_host_t* host, int index, int16_t x, int16_t y, uint8_t buttons, uint8_t active) {
    if (!host->memory || !host->info.pointer_ptr || index < 0 || index >= WC_MAX_POINTERS) return;
    uint32_t offset = host->info.pointer_ptr + (index * 8);
    *(int16_t*)(host->memory + offset + 0) = x;
    *(int16_t*)(host->memory + offset + 2) = y;
    host->memory[offset + 4] = buttons;
    host->memory[offset + 5] = active;
    host->memory[offset + 6] = 0;
    host->memory[offset + 7] = 0;
}

extern "C" void wc_host_set_time(wc_host_t* host, double time_ms, double delta_ms, uint32_t frame) {
    if (!host->memory || !host->info.time_ptr) return;
    uint32_t ptr = host->info.time_ptr;
    wc_write_f64(host->memory, ptr + WC_TIME_TIME_MS, time_ms);
    wc_write_f64(host->memory, ptr + WC_TIME_DELTA_MS, delta_ms);
    wc_write_u32(host->memory, ptr + WC_TIME_FRAME, frame);
}

// ─── Run frame ─────────────────────────────────────────────────────────

static v8::Locker* g_persistent_locker = nullptr;
static v8::Isolate::Scope* g_persistent_isolate_scope = nullptr;

extern "C" void wc_host_enter_v8(void) {
    g_persistent_locker = new v8::Locker(g_isolate);
    g_persistent_isolate_scope = new v8::Isolate::Scope(g_isolate);
}

extern "C" void wc_host_exit_v8(void) {
    delete g_persistent_isolate_scope; g_persistent_isolate_scope = nullptr;
    delete g_persistent_locker; g_persistent_locker = nullptr;
}

extern "C" void wc_host_run_frame(wc_host_t* host) {
    if (!host->fn_wc_render || host->trapped) return;

    // Locker + Isolate::Scope held persistently. Only HandleScope + Context::Scope per frame.
    v8::HandleScope handle_scope(g_isolate);
    v8::Context::Scope context_scope(ctx());

    auto state = (v8_host_state*)host->v8_state;
    v8::TryCatch try_catch(g_isolate);

    auto result = state->fn_wc_render.Get(g_isolate)->Call(
        ctx(), ctx()->Global(), 0, nullptr);

    if (result.IsEmpty() && try_catch.HasCaught()) {
        v8::String::Utf8Value err(g_isolate, try_catch.Exception());
        fprintf(stderr, "wasmcart: wc_render trapped: %s\n", *err);
        host->trapped = true;
        return;
    }

    refresh_memory(host);
}

// ─── Readback (unchanged — pure C, reads host->memory) ────────────────

extern "C" const uint8_t* wc_host_get_framebuffer(wc_host_t* host, uint32_t* width, uint32_t* height) {
    if (!host->memory || !host->info.fb_ptr) {
        *width = 0; *height = 0;
        return NULL;
    }
    *width = host->info.width;
    *height = host->info.height;
    return host->memory + host->info.fb_ptr;
}

static uint8_t* _audio_copy_buf = NULL;
static uint32_t _audio_copy_cap = 0;

extern "C" const void* wc_host_get_audio(wc_host_t* host, uint32_t* num_frames, bool* is_f32) {
    if (!host->memory || !host->info.audio_ptr || !host->info.audio_cap) {
        *num_frames = 0; *is_f32 = false;
        return NULL;
    }

    *is_f32 = (host->info.flags & WC_FLAG_AUDIO_F32) != 0;

    uint32_t write_cursor = wc_read_u32(host->memory, host->info.audio_write_ptr);
    uint32_t read_cursor = host->audio_read_cursor;
    uint32_t cap = host->info.audio_cap;

    if (write_cursor == read_cursor) { *num_frames = 0; return NULL; }

    uint32_t available;
    if (write_cursor >= read_cursor)
        available = write_cursor - read_cursor;
    else
        available = cap - read_cursor + write_cursor;

    if (available == 0) { *num_frames = 0; return NULL; }

    uint32_t needed = available * 2;

    if (*is_f32) {
        uint32_t needed_bytes = needed * sizeof(float);
        if (_audio_copy_cap < needed_bytes) {
            _audio_copy_buf = (uint8_t*)realloc(_audio_copy_buf, needed_bytes);
            _audio_copy_cap = needed_bytes;
        }
        float* out = (float*)_audio_copy_buf;
        const float* ring_f32 = (const float*)(host->memory + host->info.audio_ptr);
        for (uint32_t i = 0; i < available; i++) {
            uint32_t ring_idx = ((read_cursor + i) % cap) * 2;
            out[i * 2]     = ring_f32[ring_idx];
            out[i * 2 + 1] = ring_f32[ring_idx + 1];
        }
    } else {
        uint32_t needed_bytes = needed * sizeof(int16_t);
        if (_audio_copy_cap < needed_bytes) {
            _audio_copy_buf = (uint8_t*)realloc(_audio_copy_buf, needed_bytes);
            _audio_copy_cap = needed_bytes;
        }
        int16_t* out = (int16_t*)_audio_copy_buf;
        const int16_t* ring_i16 = (const int16_t*)(host->memory + host->info.audio_ptr);
        for (uint32_t i = 0; i < available; i++) {
            uint32_t ring_idx = ((read_cursor + i) % cap) * 2;
            out[i * 2]     = ring_i16[ring_idx];
            out[i * 2 + 1] = ring_i16[ring_idx + 1];
        }
    }

    host->audio_read_cursor = write_cursor;
    *num_frames = available;
    return _audio_copy_buf;
}

extern "C" uint8_t* wc_host_get_save_data(wc_host_t* host, uint32_t* size) {
    if (!host->memory || !host->info.save_ptr || !host->info.save_size) {
        *size = 0; return NULL;
    }
    *size = host->info.save_size;
    return host->memory + host->info.save_ptr;
}

// ─── GL / Info (unchanged) ──────────────────────────────────────────────

extern "C" bool wc_host_uses_gl(wc_host_t* host) { return host->uses_gl; }
extern "C" bool wc_host_has_trapped(wc_host_t* host) { return host->trapped; }
extern "C" void wc_host_set_gl_loader(wc_host_t* host, wc_gl_get_proc_fn loader) { host->gl_loader = loader; }
extern "C" const wc_cart_info_t* wc_host_get_cart_info(wc_host_t* host) { return &host->info; }
extern "C" const wc_manifest_t* wc_host_get_manifest(wc_host_t* host) { return &host->manifest; }
extern "C" void* wc_host_get_memory(wc_host_t* host, uint32_t* size) {
    if (size) *size = host->memory_size;
    return host->memory;
}
