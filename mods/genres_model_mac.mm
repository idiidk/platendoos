// The genres mod's style model on Core ML (see genres_model.h): models/effnet.mlmodelc in the app's resources.
#import <CoreML/CoreML.h>
#import <Foundation/Foundation.h>
#include "genres_model.h"
#include "../core/rbmod.h"

using namespace rbm;

namespace genres_backend {
namespace {
MLModel* g_model;
}

bool load() {
    @autoreleasepool {
        NSString* path = [NSString stringWithUTF8String:(plat::resourcesDir() + "/effnet.mlmodelc").c_str()];
        if (![NSFileManager.defaultManager fileExistsAtPath:path]) {
            logf("genres: no style model (%s)", path.UTF8String);
            return false;
        }
        MLModelConfiguration* cfg = [MLModelConfiguration new];
        cfg.computeUnits = cfgInt("gpu", 1) ? MLComputeUnitsCPUAndGPU : MLComputeUnitsCPUOnly;
        NSError* err = nil;
        double t = now();
        g_model = [MLModel modelWithContentsOfURL:[NSURL fileURLWithPath:path] configuration:cfg error:&err];
        if (!g_model) {
            logf("genres: loading the style model failed: %s", err.localizedDescription.UTF8String);
            return false;
        }
        logf("genres: style model loaded in %.1f s", now() - t);
        return true;
    }
}

bool predict(const float* mel, int batch, std::vector<float>& out, std::string* error) {
    @autoreleasepool {
        NSError* err = nil;
        MLMultiArray* in = [[MLMultiArray alloc] initWithDataPointer:(void*)mel shape:@[@(batch), @128, @96] dataType:MLMultiArrayDataTypeFloat32
                                                              strides:@[@(128 * 96), @96, @1] deallocator:nil error:&err];
        id<MLFeatureProvider> fp = in ? [[MLDictionaryFeatureProvider alloc] initWithDictionary:@{@"melspectrogram": in} error:&err] : nil;
        id<MLFeatureProvider> res = fp ? [g_model predictionFromFeatures:fp error:&err] : nil;
        MLMultiArray* a = [res featureValueForName:@"activations"].multiArrayValue;
        if (!a || a.count != batch * 400 || a.shape.count != 2) {
            *error = err ? err.localizedDescription.UTF8String : "unexpected output";
            return false;
        }
        out.resize((size_t)batch * 400);
        long s0 = a.strides[0].longValue, s1 = a.strides[1].longValue;
        bool half = a.dataType == MLMultiArrayDataTypeFloat16;
        [a getBytesWithHandler:^(const void* bytes, NSInteger) {
            for (int b = 0; b < batch; b++)
                for (int c = 0; c < 400; c++) {
                    long i = b * s0 + c * s1;
                    out[(size_t)b * 400 + c] = half ? (float)((const _Float16*)bytes)[i] : ((const float*)bytes)[i];
                }
        }];
        return true;
    }
}

}  // namespace genres_backend
