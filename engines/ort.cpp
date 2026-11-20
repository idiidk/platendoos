#include "ort.h"
#include "onnxruntime_c_api.h"

#include <stdlib.h>
#include <mutex>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#include <stdlib.h>
#endif

namespace ort {

struct Session {
    const OrtApi* api = nullptr;
    OrtEnv* env = nullptr;
    OrtSession* session = nullptr;
    OrtMemoryInfo* mem = nullptr;
    std::vector<std::string> inNames, outNames;
    std::vector<std::vector<int64_t>> inShapes;
    std::string provider = "CPU";
    std::mutex* runLock = nullptr;  // DirectML: one Run at a time per session (it doesn't support concurrent ones)
    std::string path;               // to open it again on the CPU when the GPU fails at run time
    int threads = 1;
    // No destructor, and open() hands out a shared_ptr that never deletes: the models live as long as the process.
    // (A session left to be released by static destruction at exit, with ONNX Runtime's own statics and worker
    // threads already going, dies on a destroyed mutex; shutdown() is for programs that want a clean exit.)
};

namespace {
std::vector<Session*>& sessions() {
    static auto* v = new std::vector<Session*>;   // never destroyed
    return *v;
}
std::mutex& sessionsLock() {
    static auto* m = new std::mutex;   // (models are opened from several threads)
    return *m;
}
void release(Session& s) {
    if (s.mem) s.api->ReleaseMemoryInfo(s.mem);
    if (s.session) s.api->ReleaseSession(s.session);
    if (s.env) s.api->ReleaseEnv(s.env);
    s.mem = nullptr, s.session = nullptr, s.env = nullptr;
}

std::string errorText(const OrtApi* api, OrtStatus* s) {
    std::string t = api->GetErrorMessage(s);
    api->ReleaseStatus(s);
    return t;
}

typedef OrtStatus*(ORT_API_CALL* AppendDml)(OrtSessionOptions*, int deviceId);
AppendDml g_appendDml;  // rekordbox's onnxruntime.dll is built with DirectML and exports this

// The runtime's API table (version <= 18, what 1.18 has), loaded once.
const OrtApi* loadApi(std::string* err) {
    static std::once_flag once;
    static const OrtApi* api;
    static std::string why;
    std::call_once(once, [] {
        typedef const OrtApiBase* (ORT_API_CALL * GetBase)(void);
        GetBase get = nullptr;
#ifdef _WIN32
        // Not by bare name: Windows has its own, older onnxruntime.dll in System32.
        wchar_t path[MAX_PATH * 2];
        DWORD n = GetModuleFileNameW(nullptr, path, (DWORD)(sizeof path / sizeof *path));
        std::wstring dll;
        if (n && n < sizeof path / sizeof *path) {
            dll = path;
            dll.resize(dll.find_last_of(L"\\/") + 1);
        }
        HMODULE h = LoadLibraryW((dll + L"onnxruntime.dll").c_str());
        if (!h) why = "onnxruntime.dll can't be loaded from rekordbox's folder";
        else {
            get = (GetBase)GetProcAddress(h, "OrtGetApiBase");
            g_appendDml = (AppendDml)GetProcAddress(h, "OrtSessionOptionsAppendExecutionProvider_DML");
        }
#else
        const char* env = getenv("ORT_DYLIB_PATH");
        void* h = dlopen(env ? env : "libonnxruntime.dylib", RTLD_NOW | RTLD_LOCAL);
        if (!h && !env) h = dlopen("libonnxruntime.so", RTLD_NOW | RTLD_LOCAL);
        if (!h) why = "libonnxruntime can't be loaded (set ORT_DYLIB_PATH)";
        else get = (GetBase)dlsym(h, "OrtGetApiBase");
#endif
        if (!get) {
            if (why.empty()) why = "onnxruntime has no OrtGetApiBase";
            return;
        }
        const OrtApiBase* base = get();
        for (int v = ORT_API_VERSION; v >= 14 && !api; v--) api = base->GetApi((uint32_t)v);
        if (!api) why = std::string("onnxruntime ") + base->GetVersionString() + " is too old";
    });
    if (!api && err) *err = why;
    return api;
}

#ifdef _WIN32
std::wstring widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}
#endif

#ifdef _WIN32
// ONNX Runtime's worker threads are started here, as threads of ours: like every thread the mods start they run
// below normal priority (core/plat_win.cpp, worker_priority), so a model on the CPU doesn't hold up rekordbox's
// drawing. (Started by ONNX Runtime itself they would run at normal priority.)
OrtCustomThreadHandle createWorker(void*, OrtThreadWorkerFn fn, void* param) {
    return (OrtCustomThreadHandle) new std::thread([fn, param] { fn(param); });
}
void joinWorker(OrtCustomThreadHandle h) {
    auto* t = (std::thread*)h;
    t->join();
    delete t;
}
#endif

// A session on the GPU (DirectML) or the CPU; *why says why not on the GPU when it was asked for.
OrtStatus* createSession(Session& s, const std::string& path, int threads, bool gpu, std::string* why) {
    const OrtApi* api = s.api;
    OrtSessionOptions* opts = nullptr;
    OrtStatus* st = api->CreateSessionOptions(&opts);
    if (!st) st = api->SetIntraOpNumThreads(opts, threads > 0 ? threads : 1);
    if (!st) st = api->SetSessionGraphOptimizationLevel(opts, ORT_ENABLE_ALL);
#ifdef _WIN32
    if (!st) st = api->SessionOptionsSetCustomCreateThreadFn(opts, createWorker);
    if (!st) st = api->SessionOptionsSetCustomJoinThreadFn(opts, joinWorker);
#endif
    if (!st && gpu) {  // DirectML needs memory patterns off and sequential execution
        st = api->DisableMemPattern(opts);
        if (!st) st = api->SetSessionExecutionMode(opts, ORT_SEQUENTIAL);
        if (!st) st = g_appendDml(opts, 0);
    }
    if (!st) {
#ifdef _WIN32
        st = api->CreateSession(s.env, widen(path).c_str(), opts, &s.session);
#else
        st = api->CreateSession(s.env, path.c_str(), opts, &s.session);
#endif
    }
    if (opts) api->ReleaseSessionOptions(opts);
    if (st && gpu && why) *why = errorText(api, st), st = nullptr, s.session = nullptr;
    return st;
}

}  // namespace

std::shared_ptr<Session> open(const std::string& path, int threads, std::string* err, bool gpu, bool gpuOnly) {
    const OrtApi* api = loadApi(err);
    if (!api) return nullptr;
    std::shared_ptr<Session> s(new Session, [](Session*) {});
    s->api = api;
    s->path = path, s->threads = threads;
    OrtStatus* st = api->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "platendoos", &s->env);
    std::string why;
    if (!st && gpu && !g_appendDml) why = "this onnxruntime has no DirectML";
    if (!st && gpu && g_appendDml) {
        st = createSession(*s, path, threads, true, &why);
        if (!st && s->session) s->provider = "GPU (DirectML)", s->runLock = new std::mutex;
    }
    if (!st && !s->session && gpu && gpuOnly) {
        if (err) *err = "no GPU session: " + why;
        release(*s);
        return nullptr;
    }
    if (!st && !s->session) {
        st = createSession(*s, path, threads, false, nullptr);
        if (gpu) s->provider = "CPU (no GPU: " + why + ")";
    }
    if (!st) st = api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &s->mem);
    OrtAllocator* alloc = nullptr;
    if (!st) st = api->GetAllocatorWithDefaultOptions(&alloc);
    size_t nIn = 0, nOut = 0;
    if (!st) st = api->SessionGetInputCount(s->session, &nIn);
    if (!st) st = api->SessionGetOutputCount(s->session, &nOut);
    for (size_t i = 0; !st && i < nIn; i++) {
        char* name = nullptr;
        st = api->SessionGetInputName(s->session, i, alloc, &name);
        if (st) break;
        s->inNames.push_back(name);
        alloc->Free(alloc, name);
        OrtTypeInfo* ti = nullptr;
        st = api->SessionGetInputTypeInfo(s->session, i, &ti);
        if (st) break;
        const OrtTensorTypeAndShapeInfo* ts = nullptr;
        std::vector<int64_t> dims;
        if (!api->CastTypeInfoToTensorInfo(ti, &ts) && ts) {
            size_t nd = 0;
            if (!api->GetDimensionsCount(ts, &nd)) {
                dims.resize(nd);
                api->GetDimensions(ts, dims.data(), nd);
            }
        }
        api->ReleaseTypeInfo(ti);
        s->inShapes.push_back(dims);
    }
    for (size_t i = 0; !st && i < nOut; i++) {
        char* name = nullptr;
        st = api->SessionGetOutputName(s->session, i, alloc, &name);
        if (st) break;
        s->outNames.push_back(name);
        alloc->Free(alloc, name);
    }
    if (st) {
        if (err) *err = errorText(api, st);
        release(*s);
        return nullptr;
    }
    {
        std::lock_guard<std::mutex> l(sessionsLock());
        sessions().push_back(s.get());
    }
    return s;
}

void close(Session& s) {
    std::lock_guard<std::mutex> l(sessionsLock());
    auto& all = sessions();
    for (size_t i = 0; i < all.size(); i++)
        if (all[i] == &s) { all.erase(all.begin() + i); break; }
    release(s);
}

void shutdown() {
    std::lock_guard<std::mutex> l(sessionsLock());
    for (Session* s : sessions()) release(*s);
    sessions().clear();
}

const std::vector<std::string>& inputNames(const Session& s) { return s.inNames; }
const std::string& provider(const Session& s) { return s.provider; }
const std::vector<std::string>& outputNames(const Session& s) { return s.outNames; }
const std::vector<int64_t>& inputShape(const Session& s, size_t i) { return s.inShapes.at(i); }

bool run(Session& s, const std::vector<Input>& in, const std::vector<std::string>& outNames,
         std::vector<std::vector<float>>& out, std::string* err, std::vector<std::vector<int64_t>>* outShapes) {
    const OrtApi* api = s.api;
    std::vector<OrtValue*> inVals(in.size(), nullptr), outVals(outNames.size(), nullptr);
    std::vector<const char*> inNames, outNamesC;
    OrtStatus* st = nullptr;
    for (size_t i = 0; !st && i < in.size(); i++) {
        size_t count = 1;
        for (int64_t d : in[i].shape) count *= (size_t)d;
        inNames.push_back(in[i].name);
        st = api->CreateTensorWithDataAsOrtValue(s.mem, (void*)in[i].data, count * sizeof(float), in[i].shape.data(),
                                                 in[i].shape.size(), ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &inVals[i]);
    }
    for (const std::string& n : outNames) outNamesC.push_back(n.c_str());
    std::unique_lock<std::mutex> lock;
    if (s.runLock) lock = std::unique_lock<std::mutex>(*s.runLock);
    if (!st)
        st = api->Run(s.session, nullptr, inNames.data(), inVals.data(), inVals.size(), outNamesC.data(), outNamesC.size(),
                      outVals.data());
    // The GPU failed at run time (driver reset, out of memory, ...): from now on this session runs on the CPU. The
    // GPU session is left alone (not released: another thread may have just read it), and the run is tried again.
    if (st && s.runLock && s.provider.rfind("GPU", 0) == 0) {
        std::string why = errorText(api, st);
        OrtSession* gpu = s.session;
        s.session = nullptr;
        st = createSession(s, s.path, s.threads, false, nullptr);
        if (st) s.session = gpu;  // (the CPU one couldn't be made either: keep reporting this error)
        else {
            s.provider = "CPU (the GPU failed: " + why + ")";
            for (OrtValue*& v : outVals) if (v) api->ReleaseValue(v), v = nullptr;
            st = api->Run(s.session, nullptr, inNames.data(), inVals.data(), inVals.size(), outNamesC.data(), outNamesC.size(),
                          outVals.data());
        }
    }
    out.resize(outNames.size());
    if (outShapes) outShapes->resize(outNames.size());
    for (size_t i = 0; !st && i < outVals.size(); i++) {
        OrtTensorTypeAndShapeInfo* ti = nullptr;
        st = api->GetTensorTypeAndShape(outVals[i], &ti);
        if (st) break;
        ONNXTensorElementDataType type;
        size_t count = 0, nd = 0;
        api->GetTensorElementType(ti, &type);
        api->GetTensorShapeElementCount(ti, &count);
        api->GetDimensionsCount(ti, &nd);
        if (outShapes) {
            (*outShapes)[i].resize(nd);
            api->GetDimensions(ti, (*outShapes)[i].data(), nd);
        }
        api->ReleaseTensorTypeAndShapeInfo(ti);
        if (type != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
            if (err) *err = "output " + outNames[i] + " is not float";
            for (OrtValue* v : outVals) if (v) api->ReleaseValue(v);
            for (OrtValue* v : inVals) if (v) api->ReleaseValue(v);
            return false;
        }
        float* data = nullptr;
        st = api->GetTensorMutableData(outVals[i], (void**)&data);
        if (!st) out[i].assign(data, data + count);
    }
    for (OrtValue* v : outVals) if (v) api->ReleaseValue(v);
    for (OrtValue* v : inVals) if (v) api->ReleaseValue(v);
    if (st) {
        if (err) *err = errorText(api, st);
        return false;
    }
    return true;
}

}  // namespace ort
