// dmltest's probe: one ONNX model through rekordbox's onnxruntime.dll (in the exe's folder) with every DirectML knob
// separately, each result compared with the CPU provider's (SNR per output). Float inputs and outputs only. README.md.
//   environment: REF=<file> keeps the CPU reference between runs, DUMP=<prefix> writes every output, ORT_LOG=0..4
//   probe.exe <model.onnx> <inputs: name=file.f32|name=noise ...> -- <config> ...
//   config: label:key=value,...   keys: path=cpu|legacy|generic  opt=0|1|2|99  meta=0|1 (generic: disable_metacommands)
//           fusion=0|1 (0: ep.dml.disable_graph_fusion=1)  dev=<device_id>  cfg.<session config key>=<value>
//           save=<file> (optimized model)  runs=<n>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <map>
#include <string>
#include <vector>
#include <chrono>
#include "onnxruntime_c_api.h"

static const OrtApi* api;
typedef OrtStatus*(ORT_API_CALL* AppendDml)(OrtSessionOptions*, int);
static AppendDml appendDml;
static double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
static std::wstring widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}
#define CHECK(st) do { OrtStatus* s_ = (st); if (s_) { printf("  ERROR: %s\n", api->GetErrorMessage(s_)); api->ReleaseStatus(s_); return false; } } while (0)

struct Tensor { std::string name; std::vector<int64_t> shape; std::vector<float> data; };

static bool runConfig(OrtEnv* env, const std::string& model, std::map<std::string, std::string> k, std::vector<Tensor>& in,
                      std::vector<Tensor>& out, double* took) {
    OrtSessionOptions* o = nullptr;
    CHECK(api->CreateSessionOptions(&o));
    CHECK(api->SetIntraOpNumThreads(o, 6));
    int opt = k.count("opt") ? atoi(k["opt"].c_str()) : 99;
    CHECK(api->SetSessionGraphOptimizationLevel(o, (GraphOptimizationLevel)opt));
    for (auto& [key, v] : k)
        if (key.rfind("cfg.", 0) == 0) CHECK(api->AddSessionConfigEntry(o, key.c_str() + 4, v.c_str()));
    if (k.count("save")) CHECK(api->SetOptimizedModelFilePath(o, widen(k["save"]).c_str()));
    std::string path = k.count("path") ? k["path"] : "legacy";
    if (path != "cpu") {
        CHECK(api->DisableMemPattern(o));
        CHECK(api->SetSessionExecutionMode(o, ORT_SEQUENTIAL));
        if (k.count("fusion") && k["fusion"] == "0") CHECK(api->AddSessionConfigEntry(o, "ep.dml.disable_graph_fusion", "1"));
        std::string dev = k.count("dev") ? k["dev"] : "0";
        if (path == "legacy") CHECK(appendDml(o, atoi(dev.c_str())));
        else {
            std::vector<const char*> keys, vals;
            std::string metaV = k.count("metaval") ? k["metaval"] : "True";
            if (!k.count("nodev")) keys.push_back("device_id"), vals.push_back(dev.c_str());
            if (k.count("meta") && k["meta"] == "0") keys.push_back("disable_metacommands"), vals.push_back(metaV.c_str());
            for (auto& [key, v] : k)
                if (key.rfind("po.", 0) == 0) keys.push_back(key.c_str() + 3), vals.push_back(v.c_str());
            CHECK(api->SessionOptionsAppendExecutionProvider(o, k.count("pname") ? k["pname"].c_str() : "DML", keys.data(), vals.data(), keys.size()));
        }
    }
    OrtSession* s = nullptr;
    double t0 = now();
    CHECK(api->CreateSession(env, widen(model).c_str(), o, &s));
    double made = now() - t0;
    api->ReleaseSessionOptions(o);
    OrtMemoryInfo* mem = nullptr;
    CHECK(api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &mem));
    OrtAllocator* alloc = nullptr;
    CHECK(api->GetAllocatorWithDefaultOptions(&alloc));
    size_t nOut = 0;
    CHECK(api->SessionGetOutputCount(s, &nOut));
    std::vector<std::string> outNames;
    for (size_t i = 0; i < nOut; i++) {
        char* n = nullptr;
        CHECK(api->SessionGetOutputName(s, i, alloc, &n));
        outNames.push_back(n);
        alloc->Free(alloc, n);
    }
    std::vector<const char*> inN, outN;
    std::vector<OrtValue*> inV(in.size(), nullptr);
    for (size_t i = 0; i < in.size(); i++) {
        inN.push_back(in[i].name.c_str());
        CHECK(api->CreateTensorWithDataAsOrtValue(mem, in[i].data.data(), in[i].data.size() * sizeof(float), in[i].shape.data(),
                                                  in[i].shape.size(), ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &inV[i]));
    }
    for (auto& n : outNames) outN.push_back(n.c_str());
    int runs = k.count("runs") ? atoi(k["runs"].c_str()) : 2;
    double best = 1e9;
    std::vector<OrtValue*> outV(nOut, nullptr);
    for (int r = 0; r < runs; r++) {
        for (auto& v : outV) if (v) api->ReleaseValue(v), v = nullptr;
        double t = now();
        CHECK(api->Run(s, nullptr, inN.data(), inV.data(), inV.size(), outN.data(), outN.size(), outV.data()));
        best = std::min(best, now() - t);
    }
    out.clear();
    for (size_t i = 0; i < nOut; i++) {
        OrtTensorTypeAndShapeInfo* ti = nullptr;
        CHECK(api->GetTensorTypeAndShape(outV[i], &ti));
        size_t count = 0, nd = 0;
        api->GetTensorShapeElementCount(ti, &count);
        api->GetDimensionsCount(ti, &nd);
        Tensor t;
        t.name = outNames[i];
        t.shape.resize(nd);
        api->GetDimensions(ti, t.shape.data(), nd);
        ONNXTensorElementDataType type;
        api->GetTensorElementType(ti, &type);
        api->ReleaseTensorTypeAndShapeInfo(ti);
        if (type == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
            float* d = nullptr;
            CHECK(api->GetTensorMutableData(outV[i], (void**)&d));
            t.data.assign(d, d + count);
        }
        out.push_back(std::move(t));
        api->ReleaseValue(outV[i]);
    }
    for (auto* v : inV) api->ReleaseValue(v);
    api->ReleaseMemoryInfo(mem);
    api->ReleaseSession(s);
    *took = best;
    printf("  session %.1f s, run %.2f s\n", made, best);
    return true;
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    wchar_t path[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, path, MAX_PATH * 2);
    std::wstring dll = path;
    dll.resize(dll.find_last_of(L"\\/") + 1);
    HMODULE h = LoadLibraryW((dll + L"onnxruntime.dll").c_str());
    if (!h) { printf("no onnxruntime.dll\n"); return 2; }
    auto get = (const OrtApiBase* (ORT_API_CALL*)(void))GetProcAddress(h, "OrtGetApiBase");
    appendDml = (AppendDml)GetProcAddress(h, "OrtSessionOptionsAppendExecutionProvider_DML");
    for (int v = ORT_API_VERSION; v >= 14 && !api; v--) api = get()->GetApi((uint32_t)v);
    printf("onnxruntime %s, legacy DML export %s\n", get()->GetVersionString(), appendDml ? "yes" : "no");
    OrtEnv* env = nullptr;
    int level = getenv("ORT_LOG") ? atoi(getenv("ORT_LOG")) : 3;
    if (api->CreateEnv((OrtLoggingLevel)level, "probe", &env)) return 2;
    std::string model = argv[1];
    std::vector<Tensor> in;
    int a = 2;
    // the model's inputs
    {
        OrtSessionOptions* o = nullptr;
        api->CreateSessionOptions(&o);
        api->SetSessionGraphOptimizationLevel(o, ORT_DISABLE_ALL);
        OrtSession* s = nullptr;
        if (OrtStatus* st = api->CreateSession(env, widen(model).c_str(), o, &s)) { printf("%s\n", api->GetErrorMessage(st)); return 2; }
        OrtAllocator* alloc = nullptr;
        api->GetAllocatorWithDefaultOptions(&alloc);
        size_t n = 0;
        api->SessionGetInputCount(s, &n);
        for (size_t i = 0; i < n; i++) {
            char* name = nullptr;
            api->SessionGetInputName(s, i, alloc, &name);
            Tensor t;
            t.name = name;
            OrtTypeInfo* ti = nullptr;
            api->SessionGetInputTypeInfo(s, i, &ti);
            const OrtTensorTypeAndShapeInfo* ts = nullptr;
            api->CastTypeInfoToTensorInfo(ti, &ts);
            size_t nd = 0;
            api->GetDimensionsCount(ts, &nd);
            t.shape.resize(nd);
            api->GetDimensions(ts, t.shape.data(), nd);
            api->ReleaseTypeInfo(ti);
            in.push_back(t);
        }
        api->ReleaseSession(s);
        api->ReleaseSessionOptions(o);
    }
    for (; a < argc && strcmp(argv[a], "--"); a++) {
        std::string s = argv[a];
        size_t eq = s.find('=');
        std::string name = s.substr(0, eq), val = s.substr(eq + 1);
        for (auto& t : in)
            if (t.name == name) {
                if (val.rfind("shape:", 0) == 0) {  // name=shape:1x2x3
                    t.shape.clear();
                    for (char* p = &val[6]; *p;) { t.shape.push_back(strtoll(p, &p, 10)); if (*p) p++; }
                    continue;
                }
                size_t count = 1;
                for (auto d : t.shape) count *= (size_t)d;
                t.data.resize(count);
                FILE* f = fopen(val.c_str(), "rb");
                if (!f || fread(t.data.data(), sizeof(float), count, f) != count) { printf("can't read %s (%zu floats)\n", val.c_str(), count); return 2; }
                fclose(f);
            }
    }
    unsigned seed = 3;
    for (auto& t : in) {
        size_t count = 1;
        for (auto d : t.shape) count *= (size_t)d;
        if (t.data.empty()) {
            t.data.resize(count);
            for (auto& v : t.data) { seed = seed * 1664525u + 1013904223u; v = ((int)(seed >> 16) - 32768) / 32768.f * 0.1f; }
        }
        printf("input %s [", t.name.c_str());
        for (auto d : t.shape) printf("%lld ", (long long)d);
        printf("] %zu floats\n", count);
    }
    std::vector<Tensor> ref, out;
    double took;
    std::string refFile = getenv("REF") ? getenv("REF") : "";
    bool haveRef = false;
    if (!refFile.empty()) {
        FILE* f = fopen(refFile.c_str(), "rb");
        if (f) {
            uint32_t n;
            fread(&n, 4, 1, f);
            ref.resize(n);
            for (auto& t : ref) { uint64_t c; fread(&c, 8, 1, f); t.data.resize(c); fread(t.data.data(), 4, c, f); }
            fclose(f);
            haveRef = true;
            printf("reference from %s\n", refFile.c_str());
        }
    }
    if (!haveRef) {
        printf("reference: CPU, opt=1\n");
        if (!runConfig(env, model, {{"path", "cpu"}, {"opt", "1"}, {"runs", "1"}}, in, ref, &took)) return 2;
        if (!refFile.empty()) {
            FILE* f = fopen(refFile.c_str(), "wb");
            uint32_t n = (uint32_t)ref.size();
            fwrite(&n, 4, 1, f);
            for (auto& t : ref) { uint64_t c = t.data.size(); fwrite(&c, 8, 1, f); fwrite(t.data.data(), 4, c, f); }
            fclose(f);
        }
    }
    for (a++; a < argc; a++) {
        std::string s = argv[a];
        size_t colon = s.find(':');
        std::string label = s.substr(0, colon);
        std::map<std::string, std::string> k;
        for (size_t p = colon + 1; p < s.size();) {
            size_t e = s.find(',', p);
            if (e == std::string::npos) e = s.size();
            std::string kv = s.substr(p, e - p);
            size_t eq = kv.find('=');
            k[kv.substr(0, eq)] = eq == std::string::npos ? "" : kv.substr(eq + 1);
            p = e + 1;
        }
        printf("== %s (%s)\n", label.c_str(), s.c_str() + colon + 1);
        if (!runConfig(env, model, k, in, out, &took)) { printf("RESULT %-28s FAILED\n", label.c_str()); continue; }
        if (const char* d = getenv("DUMP")) {   // every output and its reference: <d><label>_<i>.f32, <d>ref_<i>.f32
            for (size_t i = 0; i < out.size() && i < ref.size(); i++) {
                FILE* f = fopen((std::string(d) + label + "_" + std::to_string(i) + ".f32").c_str(), "wb");
                fwrite(out[i].data.data(), 4, out[i].data.size(), f);
                fclose(f);
                f = fopen((std::string(d) + "ref_" + std::to_string(i) + ".f32").c_str(), "wb");
                fwrite(ref[i].data.data(), 4, ref[i].data.size(), f);
                fclose(f);
                printf("  shape %s:", out[i].name.c_str());
                for (auto x : out[i].shape) printf(" %lld", (long long)x);
                printf("\n");
            }
        }
        printf("RESULT %-28s %.2f s", label.c_str(), took);
        for (size_t i = 0; i < out.size() && i < ref.size(); i++) {
            double e = 0, r = 0, mx = 0;
            size_t bad = 0;
            size_t n = std::min(out[i].data.size(), ref[i].data.size());
            for (size_t j = 0; j < n; j++) {
                double x = out[i].data[j], y = ref[i].data[j];
                if (!std::isfinite(x)) { bad++; continue; }
                e += (x - y) * (x - y), r += y * y, mx = std::max(mx, fabs(x));
            }
            printf(" | %s: SNR %.1f dB, max |x| %.3g (ref rms %.3g)%s", out[i].name.c_str(), 10 * log10((r + 1e-30) / (e + 1e-30)), mx,
                   sqrt(r / (n + 1e-9)), bad ? ", NOT FINITE" : "");
        }
        printf("\n");
    }
    return 0;
}
