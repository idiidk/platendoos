// HTDemucs on CoreML (the backend of betterstems.cpp, see betterstems_backend.h).
#import <CoreML/CoreML.h>
#import <Foundation/Foundation.h>
#include <sys/stat.h>
#include <string>
#include <vector>
#include "betterstems_backend.h"
#include "../core/rbmod.h"

using namespace rbm;

namespace betterstems_backend {

namespace {
MLModel* g_model;

NSString* packagePath() {
    return [NSString stringWithUTF8String:(plat::configDir() + "/htdemucs.mlpackage").c_str()];
}

MLMultiArray* array(void* data, NSArray<NSNumber*>* shape) {
    NSMutableArray<NSNumber*>* strides = [NSMutableArray arrayWithCapacity:shape.count];
    long s = 1;
    for (NSInteger i = shape.count - 1; i >= 0; i--) {
        [strides insertObject:@(s) atIndex:0];
        s *= shape[i].longValue;
    }
    return [[MLMultiArray alloc] initWithDataPointer:data shape:shape dataType:MLMultiArrayDataTypeFloat32 strides:strides
                                         deallocator:nil error:nil];
}

// The values of an output as contiguous floats, whatever its type and (GPU-padded) strides.
bool floats(MLMultiArray* a, size_t count, std::vector<float>& out) {
    if (!a || (size_t)a.count != count) return false;
    out.resize(count);
    NSInteger dims = a.shape.count, last = a.shape[dims - 1].integerValue, rows = (NSInteger)count / last;
    std::vector<NSInteger> shape(dims), strides(dims);
    for (NSInteger d = 0; d < dims; d++) shape[d] = a.shape[d].integerValue, strides[d] = a.strides[d].integerValue;
    bool half = a.dataType == MLMultiArrayDataTypeFloat16;
    if (!half && a.dataType != MLMultiArrayDataTypeFloat32) return false;
    __block bool ok = true;
    [a getBytesWithHandler:^(const void* bytes, NSInteger size) {
        for (NSInteger row = 0; row < rows && ok; row++) {
            NSInteger offset = 0, rest = row;  // the row's element offset: its index over all but the last dimension
            for (NSInteger d = dims - 2; d >= 0; d--) offset += (rest % shape[d]) * strides[d], rest /= shape[d];
            if ((offset + (last - 1) * strides[dims - 1] + 1) * (half ? 2 : 4) > size) {
                ok = false;
                break;
            }
            float* o = out.data() + (size_t)row * last;
            if (half)
                for (NSInteger i = 0; i < last; i++) o[i] = (float)((const _Float16*)bytes)[offset + i * strides[dims - 1]];
            else
                for (NSInteger i = 0; i < last; i++) o[i] = ((const float*)bytes)[offset + i * strides[dims - 1]];
        }
    }];
    return ok;
}
}  // namespace

bool installed() { return [NSFileManager.defaultManager fileExistsAtPath:packagePath()]; }

// The model compiled into the app (models/htdemucs.mlmodelc -> Contents/Resources/rbmods), so a copied app is
// complete; else the package in Application Support, compiled once (to ~/Library/Caches/rekordbox-mods/htdemucs.mlmodelc).
// Both give the same version (the weights' size), so stems already cached stay valid.
bool load(std::string* version) {
    @autoreleasepool {
#if RBM_MAC_X86
        // An Intel Mac, as Windows (betterstems_ort.cpp): HTDemucs only on a GPU that is fast enough (betterstems.cpp
        // times it); with gpu=0 rekordbox's own engine separates, unless betterstems_slow=1.
        bool gpu = cfgInt("gpu", 1) != 0;
        if (!gpu && !cfgInt("betterstems_slow", 0)) {
            logf("betterstems: gpu=0, and HTDemucs is slow on the CPU: rekordbox's own engine separates "
                 "(betterstems_slow=1 for HTDemucs anyway)");
            return false;
        }
#endif
        NSFileManager* fm = NSFileManager.defaultManager;
        NSError* err = nil;
        NSString* compiled = [NSString stringWithUTF8String:(plat::resourcesDir() + "/htdemucs.mlmodelc").c_str()];
        if (![fm fileExistsAtPath:compiled]) compiled = nil;
        NSString* weights = [compiled stringByAppendingPathComponent:@"weights/weight.bin"];
        if (!compiled) {
            NSString* pkg = packagePath();
            weights = [pkg stringByAppendingPathComponent:@"Data/com.apple.CoreML/weights/weight.bin"];
            compiled = [NSString stringWithUTF8String:(plat::cacheDir() + "/htdemucs.mlmodelc").c_str()];
            NSDate* pkgDate = [fm attributesOfItemAtPath:pkg error:nil].fileModificationDate;
            if (!pkgDate) return false;
            NSDate* compiledDate = [fm attributesOfItemAtPath:compiled error:nil].fileModificationDate;
            if (!compiledDate || [compiledDate compare:pkgDate] == NSOrderedAscending) {
                double t = now();
                NSURL* tmp = [MLModel compileModelAtURL:[NSURL fileURLWithPath:pkg] error:&err];
                if (!tmp) {
                    logf("betterstems: compiling the model failed: %s", err.localizedDescription.UTF8String);
                    return false;
                }
                [fm createDirectoryAtPath:compiled.stringByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:nil];
                [fm removeItemAtPath:compiled error:nil];
                [fm moveItemAtURL:tmp toURL:[NSURL fileURLWithPath:compiled] error:nil];
                logf("betterstems: compiled the model in %.1f s", now() - t);
            }
        }
        MLModelConfiguration* cfg = [MLModelConfiguration new];
        cfg.computeUnits = MLComputeUnitsCPUAndGPU;  // the Neural Engine can't compile it
#if RBM_MAC_X86
        if (!gpu) cfg.computeUnits = MLComputeUnitsCPUOnly;
#endif
        double t = now();
        MLModel* m = [MLModel modelWithContentsOfURL:[NSURL fileURLWithPath:compiled] configuration:cfg error:&err];
        if (!m) {
            logf("betterstems: loading the model failed: %s", err.localizedDescription.UTF8String);
            return false;
        }
        struct stat st = {};
        stat(weights.fileSystemRepresentation, &st);
        *version = "htdemucs-" + std::to_string((long long)st.st_size);
        g_model = m;
        logf("betterstems: HTDemucs loaded in %.1f s from %s%s", now() - t, compiled.UTF8String,
             RBM_MAC_X86 ? (cfg.computeUnits == MLComputeUnitsCPUOnly ? " on the CPU" : " for the GPU") : "");
        return true;
    }
}

bool predict(const float* mix, int length, const float* mag, int bins, int frames, int sources,
             std::vector<float>& spec, std::vector<float>& wave, std::string* error) {
    @autoreleasepool {
        NSError* err = nil;
        id<MLFeatureProvider> in = [[MLDictionaryFeatureProvider alloc]
            initWithDictionary:@{@"mix": array((void*)mix, @[@1, @2, @(length)]),
                                 @"mag": array((void*)mag, @[@1, @4, @(bins), @(frames)])}
                         error:&err];
        id<MLFeatureProvider> res = in ? [g_model predictionFromFeatures:in error:&err] : nil;
        if (!res || !floats([res featureValueForName:@"spec"].multiArrayValue, (size_t)sources * 4 * bins * frames, spec) ||
            !floats([res featureValueForName:@"wave"].multiArrayValue, (size_t)sources * 2 * length, wave)) {
            *error = err ? err.localizedDescription.UTF8String : "unexpected output";
            return false;
        }
        return true;
    }
}

void unload() { g_model = nil; }

}  // namespace betterstems_backend
