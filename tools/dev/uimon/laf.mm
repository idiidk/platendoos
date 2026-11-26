#include "rbmod.h"
using namespace rbm;
extern "C" void rbm_experiment() {  // (runs on the main thread)
    void* mc = at<void* (*)()>(symbol("__ZN13MainComponent11getInstanceEv"))();
    void* top = at<void* (*)(void*)>(symbol("__ZNK4juce9Component20getTopLevelComponentEv"))(mc);
    for (int i = 0; i < 4; i++) {
        double t0 = plat::monotonic();
        at<void (*)(void*)>(symbol("__ZN4juce9Component21sendLookAndFeelChangeEv"))(top);
        logf("laf: whole-window look-and-feel broadcast %.1f ms", (plat::monotonic() - t0) * 1000);
    }
}
