#pragma once
#include <mutex>
#include <atomic>
#include "FG/Upscaler_Inputs_Dx12.h"
#include <native/VkPresentBridge.h>

// Explicit, opt-in producer contract. A normal game's native DLAA/SR calls never
// enter here. Feeder has an already rendered picture and only needs the NR seam;
// using a second zero-jitter DLAA reconstruction as its carrier corrupts detail.
namespace FeederFinishedPicture
{
inline constexpr const char* Request = "OptiScaler.Feeder.FinishedPicture";
inline constexpr const char* InputState = "OptiScaler.Feeder.InputState";
inline constexpr const char* Accepted = "OptiScaler.Feeder.FinishedPicture.Accepted";

inline void RetainInteropWrapperCode(ID3D12GraphicsCommandList* cmd)
{
    // In Vulkan interop ReShade also wraps the private D3D12 objects. Its
    // Vulkan layer can unload before NGX's process-detach cleanup invokes a
    // retained object's vtable. Keep that CODE owner mapped until process exit;
    // resource ownership/release and all ordinary game NGX calls are unchanged.
    const void* address = (*reinterpret_cast<void***>(cmd))[0];
    static std::atomic<const void*> inspected { nullptr };
    if (inspected.load(std::memory_order_relaxed) == address)
        return;
    HMODULE owner = nullptr;
    wchar_t path[MAX_PATH] {};
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(address), &owner) &&
        GetModuleFileNameW(owner, path, MAX_PATH) &&
        (wcsstr(path, L"ReShade") || wcsstr(path, L"reshade")))
    {
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                               reinterpret_cast<LPCWSTR>(address), &owner))
            LOG_INFO("Feeder finished picture: retaining interop wrapper code until process exit ({})",
                     std::filesystem::path(path).filename().string());
    }
    inspected.store(address, std::memory_order_relaxed);
}

inline bool Requested(const NVSDK_NGX_Parameter* params)
{
    unsigned int version = 0;
    return params && params->Get(Request, &version) == NVSDK_NGX_Result_Success && version == 1;
}

inline NVSDK_NGX_Result Evaluate(ID3D12GraphicsCommandList* cmd, NVSDK_NGX_Parameter* params,
                                IFeature_Dx12* feature = nullptr)
{
    ID3D12Resource* color = nullptr;
    ID3D12Resource* output = nullptr;
    unsigned int width = 0, height = 0, sourceState = 0;
    params->Set(Accepted, 0u);
    params->Get(NVSDK_NGX_Parameter_Color, (void**) &color);
    params->Get(NVSDK_NGX_Parameter_Output, (void**) &output);
    params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &width);
    params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &height);
    params->Get(InputState, &sourceState);

    const auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const auto both = read | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    if (!cmd || !color || !output || color == output || !width || !height ||
        (sourceState != unsigned(read) && sourceState != unsigned(both)))
        return NVSDK_NGX_Result_FAIL_InvalidParameter;
    const auto a = color->GetDesc(), b = output->GetDesc();
    if (a.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || b.Dimension != a.Dimension ||
        a.Width != width || a.Height != height || b.Width != width || b.Height != height ||
        a.Format != b.Format || a.SampleDesc.Count != 1 || b.SampleDesc.Count != 1 ||
        a.DepthOrArraySize != 1 || b.DepthOrArraySize != 1 || a.MipLevels != 1 || b.MipLevels != 1 ||
        !(b.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
    {
        LOG_ERROR("Feeder finished picture: incompatible native-size resources; refusing DLAA fallback");
        return NVSDK_NGX_Result_FAIL_InvalidParameter;
    }

    VkPresentBridge::NoteFeederInputs();

    ID3D12Resource* inputs[4] = { color, nullptr, nullptr, nullptr };
    params->Get(NVSDK_NGX_Parameter_Depth, (void**) &inputs[1]);
    params->Get(NVSDK_NGX_Parameter_MotionVectors, (void**) &inputs[2]);
    params->Get(NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask, (void**) &inputs[3]);
    auto transition = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                          D3D12_RESOURCE_STATES after) {
        if (!resource || before == after) return;
        D3D12_RESOURCE_BARRIER bar {};
        bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bar.Transition.pResource = resource;
        bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        bar.Transition.StateBefore = before;
        bar.Transition.StateAfter = after;
        cmd->ResourceBarrier(1, &bar);
    };
    RetainInteropWrapperCode(cmd);
    // Normalize the same-device producer's combined SRV states for NR, then
    // restore exactly what its caller declared. Host/interop already use read.
    for (auto input : inputs) transition(input, D3D12_RESOURCE_STATES(sourceState), read);

    DlssNr::EvaluateBeforeUpscale(cmd, params);
    transition(color, read, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    cmd->CopyResource(output, color);
    transition(color, D3D12_RESOURCE_STATE_COPY_SOURCE, read);
    transition(output, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    DlssNr::EvaluateAfterUpscale(cmd, params);

    // Bypassing the second DLAA reconstruction must not bypass the ordinary
    // upscaler-input seam that supplies depth and motion to frame generation.
    // Capture Output after NR, so interpolated and real frames use the same
    // enhanced picture. These copies run on Feeder's own command list; Vulkan's
    // output/present semaphore chain orders them before the bridge presents.
    if (feature && State::Instance().currentFG &&
        State::Instance().activeFgInput == FGInput::Upscaler && Config::Instance()->IsFGEnabled())
    {
        const auto& cfg = *Config::Instance();
        const auto mvState = D3D12_RESOURCE_STATES(cfg.MVResourceBarrier.value_or(read));
        const auto depthState = D3D12_RESOURCE_STATES(cfg.DepthResourceBarrier.value_or(read));
        const auto outputState = D3D12_RESOURCE_STATES(
            cfg.OutputResourceBarrier.value_or(D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
        transition(inputs[2], read, mvState);
        transition(inputs[1], read, depthState);
        transition(output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, outputState);
        UpscalerInputsDx12::UpscaleStart(cmd, params, feature);
        UpscalerInputsDx12::UpscaleEnd(cmd, params, feature);
        transition(output, outputState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        transition(inputs[1], depthState, read);
        transition(inputs[2], mvState, read);
        static std::atomic<unsigned> fgFrames { 0 };
        const auto n = ++fgFrames;
        if (n == 1 || n % 300 == 0)
        {
            LOG_INFO("Feeder finished picture: frame generation input forwarded, frame {}, {}x{} guides", n, width, height);
            if (State::Instance().activeFgOutput == FGOutput::XeFG)
            {
                const auto shown = State::Instance().xefgShown.load(std::memory_order_relaxed);
                LOG_INFO("Feeder XeFG delivery totals: {} presents, {} frames sent to the presenter",
                         shown & ((1ull << 24) - 1), shown >> 24);
            }
        }
    }

    for (auto input : inputs) transition(input, read, D3D12_RESOURCE_STATES(sourceState));
    params->Set(Accepted, 1u);
    static std::once_flag reported;
    std::call_once(reported, [] {
        LOG_INFO("Feeder finished picture contract v1: native colour copied, ordinary DLAA/SR bypassed; NR uses the supplied guides");
    });
    return NVSDK_NGX_Result_Success;
}
} // namespace FeederFinishedPicture
