#pragma once

#include "SysUtils.h"
#include "OptiTypes.h"

// Makes FFX use the FSR 4 model selected in the config, e.g. INT8 on a GPU that also runs FP8
//
// Every model has its own FFX provider class, the classes only differ in the device support check
// and FFX uses the first one that reports support. Answering that check selects the model
class FSR4Models
{
  public:
    // Safe to call more than once for the same module
    static void Hook(HMODULE module);

    // True when the module has several model providers and their support checks are patched, the byte pattern model
    // selection of FSR4ModelSelection isn't needed (and can't work, it misses new builds) for these
    static bool SelectsModels(HMODULE module);
    static bool AnyModuleSelectsModels();

    // Newest module whose models can be selected, nullptr when there is none
    static HMODULE SelectingModule();

    static bool ModuleHasModel(HMODULE module, FSR4Model model);
    static bool CanGpuRun(FSR4Model model);

    // Models of the module that can be switched between, empty when there is nothing to switch
    static std::vector<FSR4Model> SelectableModels(HMODULE module);

    // Model the module runs, from the latest support checks FFX made
    static std::optional<FSR4Model> ActiveModel(HMODULE module);

    static const char* Name(FSR4Model model);
};
