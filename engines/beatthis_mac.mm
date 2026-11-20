// Beat This! network on CoreML (the backend of beatthis.cpp, see beatthis_backend.h).
#include "beatthis.h"
#include "beatthis_backend.h"

#import <CoreML/CoreML.h>
#import <Foundation/Foundation.h>

#include <mutex>

namespace beatthis {

namespace {
constexpr int kChunk = 1500;   // beat_this Spect2Frames default
}  // namespace

struct Model::Impl {
    MLModel* model = nil;
    NSString *inName = nil, *beatName = nil, *downName = nil;
    MLMultiArrayDataType inType = MLMultiArrayDataTypeFloat32;
    NSArray<NSNumber*>* inShape = nil;
    int chunk = kChunk;
    std::mutex mu;   // serialises predict calls
};

std::string Model::device() const { return "Core ML"; }

bool Model::load(const std::string& path, std::string* err, ComputeUnits units) {
    @autoreleasepool {
        auto fail = [&](const std::string& m) { if (err) *err = m; impl_.reset(); return false; };
        NSError* e = nil;
        NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()] isDirectory:YES];
        MLModelConfiguration* cfg = [MLModelConfiguration new];
        switch (units) {
            case ComputeUnits::CpuOnly: cfg.computeUnits = MLComputeUnitsCPUOnly; break;
            case ComputeUnits::CpuAndGpu: cfg.computeUnits = MLComputeUnitsCPUAndGPU; break;
            case ComputeUnits::All: cfg.computeUnits = MLComputeUnitsAll; break;
            case ComputeUnits::CpuAndNeuralEngine: cfg.computeUnits = MLComputeUnitsCPUAndNeuralEngine; break;
        }
        MLModel* m = [MLModel modelWithContentsOfURL:url configuration:cfg error:&e];
        if (!m) return fail(std::string("load failed: ") + (e ? e.localizedDescription.UTF8String : "?"));
        auto impl = std::make_shared<Impl>();
        impl->model = m;
        NSDictionary<NSString*, MLFeatureDescription*>* ins = m.modelDescription.inputDescriptionsByName;
        if (ins.count != 1) return fail("model must have exactly one input");
        impl->inName = ins.allKeys.firstObject;
        MLMultiArrayConstraint* c = ins[impl->inName].multiArrayConstraint;
        if (!c) return fail("input is not a multiarray");
        impl->inType = c.dataType;
        impl->inShape = c.shape;
        if (c.shape.count < 2 || c.shape.lastObject.intValue != kMels) return fail("unexpected input shape");
        impl->chunk = c.shape[c.shape.count - 2].intValue;
        for (NSString* k in m.modelDescription.outputDescriptionsByName) {
            if ([k.lowercaseString containsString:@"down"]) impl->downName = k;
            else if ([k.lowercaseString containsString:@"beat"]) impl->beatName = k;
        }
        if (!impl->beatName || !impl->downName) return fail("cannot identify beat/downbeat outputs");
        impl_ = impl;
        return true;
    }
}

// Run the network on one chunk of `len` (<= chunk) frames, zero-extended to the model's fixed length.
// Writes `len` beat / downbeat logits.
bool predictChunk(Model::Impl& I, const float* mel, int len, float* beatOut, float* downOut) {
    @autoreleasepool {
        NSError* e = nil;
        MLMultiArray* in = [[MLMultiArray alloc] initWithShape:I.inShape dataType:I.inType error:&e];
        if (!in) return false;
        NSUInteger rank = I.inShape.count;
        [in getMutableBytesWithHandler:^(void* ptr, NSInteger size, NSArray<NSNumber*>* strides) {
            long st = strides[rank - 2].longValue, sf = strides[rank - 1].longValue;
            if (I.inType == MLMultiArrayDataTypeFloat32) {
                float* p = (float*)ptr;
                memset(p, 0, size);
                for (int t = 0; t < len; t++) {
                    if (sf == 1) memcpy(p + t * st, mel + (size_t)t * kMels, kMels * sizeof(float));
                    else for (int f = 0; f < kMels; f++) p[t * st + f * sf] = mel[(size_t)t * kMels + f];
                }
            } else {   // Float16
                _Float16* p = (_Float16*)ptr;
                memset(p, 0, size);
                for (int t = 0; t < len; t++)
                    for (int f = 0; f < kMels; f++) p[t * st + f * sf] = (_Float16)mel[(size_t)t * kMels + f];
            }
        }];
        MLDictionaryFeatureProvider* fp = [[MLDictionaryFeatureProvider alloc]
            initWithDictionary:@{I.inName: [MLFeatureValue featureValueWithMultiArray:in]} error:&e];
        id<MLFeatureProvider> out;
        {
            std::lock_guard<std::mutex> lk(I.mu);
            out = [I.model predictionFromFeatures:fp error:&e];
        }
        if (!out) return false;
        auto grab = [&](NSString* name, float* dst) {
            MLMultiArray* a = [out featureValueForName:name].multiArrayValue;
            if (!a) return false;
            long st = a.strides.lastObject.longValue;
            NSInteger total = a.count;
            if (total < len) return false;
            [a getBytesWithHandler:^(const void* ptr, NSInteger size) {
                if (a.dataType == MLMultiArrayDataTypeFloat16) {
                    const _Float16* p = (const _Float16*)ptr;
                    for (int t = 0; t < len; t++) dst[t] = (float)p[t * st];
                } else if (a.dataType == MLMultiArrayDataTypeFloat32) {
                    const float* p = (const float*)ptr;
                    for (int t = 0; t < len; t++) dst[t] = p[t * st];
                } else {
                    const double* p = (const double*)ptr;
                    for (int t = 0; t < len; t++) dst[t] = (float)p[t * st];
                }
            }];
            return true;
        };
        return grab(I.beatName, beatOut) && grab(I.downName, downOut);
    }
}

int chunkFrames(const Model::Impl& impl) { return impl.chunk; }

}  // namespace beatthis
