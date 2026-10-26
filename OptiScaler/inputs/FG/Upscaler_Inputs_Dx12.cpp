#include "pch.h"
#include "Upscaler_Inputs_Dx12.h"
#include <hudfix/Hudfix_Dx12.h>
#include <resource_tracking/ResTrack_dx12.h>
#include "shaders/depth_scale/DS_Dx12.h"
#include "MathUtils.h"

#include "shaders/dlssnr/DlssNr_Views.h"

using namespace OptiMath;
static DS_Dx12* DepthScale = nullptr;

namespace
{
namespace Views = DlssNr::Views;

// Games that draw several views into one allocation (a triple-screen rig: one evaluate per screen, sometimes one more for
// the combined picture) call the upscaler several times per frame. Frame generation works on the one stitched back
// buffer, so it takes one evaluate per game frame and the whole depth and motion allocations, which hold every screen
// side by side. Without this, every evaluate started a frame of its own and handed one screen's corner as the picture.
Views::Detector FgViews;
bool ViewFeedSkipped = false;

struct MultiViewFeed
{
    bool active = false; // several views are drawn
    bool skip = false;   // ... and this evaluate is not the one frame generation takes
    UINT outputWidth = 0;
    UINT outputHeight = 0;
};

ID3D12Resource* GetParamResource(NVSDK_NGX_Parameter* params, const char* name)
{
    ID3D12Resource* resource = nullptr;
    if (params->Get(name, &resource) != NVSDK_NGX_Result_Success)
        params->Get(name, (void**) &resource);
    return resource;
}

MultiViewFeed JudgeFgViews(NVSDK_NGX_Parameter* params)
{
    MultiViewFeed feed;

    ID3D12Resource* color = GetParamResource(params, NVSDK_NGX_Parameter_Color);
    if (color == nullptr)
        return feed;

    const D3D12_RESOURCE_DESC desc = color->GetDesc();
    unsigned int width = 0, height = 0, baseX = 0, baseY = 0;
    params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &width);
    params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &height);
    params->Get(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, &baseX);
    params->Get(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, &baseY);

    if (width == 0 && height == 0)
    {
        width = (unsigned int) desc.Width;
        height = desc.Height;
    }

    const Views::Seen seen { (unsigned int) desc.Width, desc.Height, (int) desc.Format, { baseX, baseY, width, height } };
    const auto result = FgViews.Note(State::Instance().frameCount, seen, 0);

    if (result.latchedNow)
        LOG_INFO("(FG) the game draws several views into one {}x{} Color allocation; frame generation takes one evaluate "
                 "per frame and the whole depth and motion allocations",
                 seen.allocW, seen.allocH);
    if (result.unlatchedNow)
        LOG_INFO("(FG) one view per frame again");

    const Views::Snapshot status = FgViews.Status();
    if (!status.active)
        return feed;

    feed.active = true;

    // The frame's last view (the combined picture when the game draws it, else the rightmost screen): the depth and
    // motion of every screen are written by then, and it comes once per game frame whenever the game presents.
    feed.skip = result.slot < 0 || result.slot != FgViews.FeedSlot();

    if (ID3D12Resource* output = GetParamResource(params, NVSDK_NGX_Parameter_Output))
    {
        const auto outDesc = output->GetDesc();
        feed.outputWidth = (UINT) outDesc.Width;
        feed.outputHeight = outDesc.Height;
    }

    // No output to size the picture by: leave this frame to the single-view path rather than guess.
    if (feed.outputWidth == 0 || feed.outputHeight == 0)
        feed = {};

    return feed;
}
} // namespace

void UpscalerInputsDx12::Init(ID3D12Device* device)
{
    if (State::Instance().activeFgInput != FGInput::Upscaler)
        return;

    _device = device;
}

void UpscalerInputsDx12::Reset()
{
    if (State::Instance().currentFG == nullptr || State::Instance().activeFgInput != FGInput::Upscaler ||
        _device == nullptr)
        return;

    if (Config::Instance()->FGHUDFix.value_or_default())
        Hudfix_Dx12::ResetCounters();
}

void UpscalerInputsDx12::UpscaleStart(ID3D12GraphicsCommandList* InCmdList, NVSDK_NGX_Parameter* InParameters,
                                      IFeature_Dx12* feature)
{
    Hudfix_Dx12::SetSkipStatus(true);

    // FSR Camera values
    float cameraNear = 0.0f;
    float cameraFar = 0.0f;
    float cameraVFov = 0.0f;
    float meterFactor = 0.0f;
    float mvScaleX = 0.0f;
    float mvScaleY = 0.0f;
    float jitterX = 0.0f;
    float jitterY = 0.0f;

    float tempCameraNear = 0.0f;
    float tempCameraFar = 0.0f;

    auto& state = State::Instance();
    auto& cfg = *Config::Instance();
    const auto& ngxParams = *InParameters;

    ngxParams.Get(OptiKeys::FSR_NearPlane, &tempCameraNear);
    ngxParams.Get(OptiKeys::FSR_FarPlane, &tempCameraFar);

    if (!cfg.FsrUseFsrInputValues.value_or_default() || (tempCameraNear == 0.0f && tempCameraFar == 0.0f))
    {
        if (feature->DepthInverted())
        {
            cameraFar = cfg.FsrCameraNear.value_or_default();
            cameraNear = cfg.FsrCameraFar.value_or_default();
        }
        else
        {
            cameraFar = cfg.FsrCameraFar.value_or_default();
            cameraNear = cfg.FsrCameraNear.value_or_default();
        }
    }
    else
    {
        cameraNear = tempCameraNear;
        cameraFar = tempCameraFar;
    }

    if (!cfg.FsrUseFsrInputValues.value_or_default() ||
        ngxParams.Get(OptiKeys::FSR_CameraFovVertical, &cameraVFov) != NVSDK_NGX_Result_Success)
    {
        if (cfg.FsrVerticalFov.has_value())
            cameraVFov = GetRadiansFromDeg(cfg.FsrVerticalFov.value());
        else if (cfg.FsrHorizontalFov.value_or_default() > 0.0f)
        {
            const float hFovRad = GetRadiansFromDeg(cfg.FsrHorizontalFov.value());
            cameraVFov =
                GetVerticalFovFromHorizontal(hFovRad, (float) feature->TargetWidth(), (float) feature->TargetHeight());
        }
        else
            cameraVFov = GetRadiansFromDeg(60);
    }

    if (!cfg.FsrUseFsrInputValues.value_or_default())
        ngxParams.Get(OptiKeys::FSR_ViewSpaceToMetersFactor, &meterFactor);

    State::Instance().lastFsrCameraFar = cameraFar;
    State::Instance().lastFsrCameraNear = cameraNear;

    auto fg = State::Instance().currentFG;

    ViewFeedSkipped = false;

    if (fg == nullptr || State::Instance().activeFgInput != FGInput::Upscaler || _device == nullptr)
        return;

    const MultiViewFeed multi = JudgeFgViews(InParameters);
    ViewFeedSkipped = multi.skip;
    if (multi.skip)
        return;

    // With several views the picture is the shared output, not this feature's view of it.
    const UINT pictureWidth = multi.active ? multi.outputWidth : feature->DisplayWidth();
    const UINT pictureHeight = multi.active ? multi.outputHeight : feature->DisplayHeight();

    FG_Constants fgConstants {};
    fgConstants.displayWidth = pictureWidth;
    fgConstants.displayHeight = pictureHeight;

    if (feature->IsHdr())
        fgConstants.flags |= FG_Flags::Hdr;

    if (feature->DepthInverted())
        fgConstants.flags |= FG_Flags::InvertedDepth;

    if (feature->JitteredMV())
        fgConstants.flags |= FG_Flags::JitteredMVs;

    if (!feature->LowResMV())
        fgConstants.flags |= FG_Flags::DisplayResolutionMVs;

    if (Config::Instance()->FGAsync.value_or_default())
        fgConstants.flags |= FG_Flags::Async;

    fg->EvaluateState(_device, fgConstants);

    int reset = 0;
    InParameters->Get(NVSDK_NGX_Parameter_Reset, &reset);

    InParameters->Get(NVSDK_NGX_Parameter_MV_Scale_X, &mvScaleX);
    InParameters->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &mvScaleY);
    InParameters->Get(NVSDK_NGX_Parameter_Jitter_Offset_X, &jitterX);
    InParameters->Get(NVSDK_NGX_Parameter_Jitter_Offset_Y, &jitterY);

    fg->StartNewFrame();

    auto aspectRatio = (float) pictureWidth / (float) pictureHeight;
    fg->SetCameraValues(cameraNear, cameraFar, cameraVFov, aspectRatio, meterFactor);
    fg->SetFrameTimeDelta(State::Instance().lastFGFrameTime);
    fg->SetMVScale(mvScaleX, mvScaleY);
    fg->SetJitter(jitterX, jitterY);
    fg->SetReset(reset);
    fg->SetInterpolationRect(pictureWidth, pictureHeight);

    Hudfix_Dx12::UpscaleStart();

    // FG Prepare
    UINT frameIndex;
    if (!State::Instance().isShuttingDown && fg->IsActive() && Config::Instance()->IsFGEnabled() &&
        State::Instance().currentSwapchain != nullptr)
    {
        // Wait for present
        if (fg->Mutex.getOwner() == 2)
        {
            LOG_TRACE("Waiting for present!");
            fg->Mutex.lock(4);
            fg->Mutex.unlockThis(4);
        }

        bool allocatorReset = false;
        frameIndex = fg->GetIndex();

        ID3D12GraphicsCommandList* commandList = nullptr;
        commandList = InCmdList;

        LOG_DEBUG("(FG) copy buffers for fgUpscaledImage[{}], frame: {}", frameIndex, fg->FrameCount());

        ID3D12Resource* paramVelocity = nullptr;
        if (InParameters->Get(NVSDK_NGX_Parameter_MotionVectors, &paramVelocity) != NVSDK_NGX_Result_Success)
            InParameters->Get(NVSDK_NGX_Parameter_MotionVectors, (void**) &paramVelocity);

        if (paramVelocity != nullptr)
        {
            Dx12Resource setResource {};
            setResource.type = FG_ResourceType::Velocity;
            setResource.cmdList = commandList;
            setResource.resource = paramVelocity;
            setResource.state = (D3D12_RESOURCE_STATES) Config::Instance()->MVResourceBarrier.value_or(
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            setResource.validity = FG_ResourceValidity::ValidNow;

            if (multi.active)
            {
                // Every screen's motion, side by side as the game packed them.
                const auto mvDesc = paramVelocity->GetDesc();
                setResource.width = (UINT) mvDesc.Width;
                setResource.height = mvDesc.Height;
            }
            else if (feature->LowResMV())
            {
                setResource.width = feature->RenderWidth();
                setResource.height = feature->RenderHeight();
            }
            else
            {
                setResource.width = feature->TargetWidth();
                setResource.height = feature->TargetHeight();
            }

            fg->SetResource(&setResource);
        }

        ID3D12Resource* paramDepth = nullptr;
        if (InParameters->Get(NVSDK_NGX_Parameter_Depth, &paramDepth) != NVSDK_NGX_Result_Success)
            InParameters->Get(NVSDK_NGX_Parameter_Depth, (void**) &paramDepth);

        if (paramDepth != nullptr)
        {
            auto done = false;

            // Depth Scale sizes its buffer by one view; with several views the game's depth goes as it is.
            if (Config::Instance()->FGEnableDepthScale.value_or_default() && !multi.active)
            {
                if (DepthScale == nullptr)
                    DepthScale = new DS_Dx12("Depth Scale", _device);

                if (DepthScale->CreateBufferResource(_device, paramDepth, feature->DisplayWidth(),
                                                     feature->DisplayHeight(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS) &&
                    DepthScale->Buffer() != nullptr)
                {
                    DepthScale->SetBufferState(InCmdList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

                    if (DepthScale->Dispatch(InCmdList, paramDepth, DepthScale->Buffer()))
                    {
                        Dx12Resource setResource {};
                        setResource.type = FG_ResourceType::Depth;
                        setResource.cmdList = commandList;
                        setResource.resource = DepthScale->Buffer();
                        setResource.width = feature->RenderWidth();
                        setResource.height = feature->RenderHeight();
                        setResource.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                        setResource.validity = FG_ResourceValidity::UntilPresent;

                        fg->SetResource(&setResource);

                        done = true;
                    }
                }
            }

            if (!done)
            {
                Dx12Resource setResource {};
                setResource.type = FG_ResourceType::Depth;
                setResource.cmdList = commandList;
                setResource.resource = paramDepth;
                if (multi.active)
                {
                    const auto depthDesc = paramDepth->GetDesc();
                    setResource.width = (UINT) depthDesc.Width;
                    setResource.height = depthDesc.Height;
                }
                else
                {
                    setResource.width = feature->RenderWidth();
                    setResource.height = feature->RenderHeight();
                }
                setResource.state = (D3D12_RESOURCE_STATES) Config::Instance()->DepthResourceBarrier.value_or(
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                setResource.validity = FG_ResourceValidity::ValidNow;

                fg->SetResource(&setResource);
            }
        }

        LOG_DEBUG("(FG) copy buffers done, frame: {0}", fg->FrameCount());
    }
}

void UpscalerInputsDx12::UpscaleEnd(ID3D12GraphicsCommandList* InCmdList, NVSDK_NGX_Parameter* InParameters,
                                    IFeature_Dx12* feature)
{
    Hudfix_Dx12::SetSkipStatus(false);

    auto fg = State::Instance().currentFG;

    if (fg == nullptr || State::Instance().activeFgInput != FGInput::Upscaler || _device == nullptr ||
        ViewFeedSkipped)
        return;

    // FG Dispatch
    if (fg->IsActive() && Config::Instance()->IsFGEnabled() && State::Instance().currentSwapchain != nullptr)
    {
        if (Config::Instance()->FGHUDFix.value_or_default())
        {
            // For signal after mv & depth copies
            Hudfix_Dx12::UpscaleEnd(feature->FrameCount(), State::Instance().lastFGFrameTime);

            ID3D12Resource* output = nullptr;
            if (InParameters->Get(NVSDK_NGX_Parameter_Output, &output) != NVSDK_NGX_Result_Success)
                InParameters->Get(NVSDK_NGX_Parameter_Output, (void**) &output);

            ResourceInfo info {};
            auto desc = output->GetDesc();
            info.buffer = output;
            info.width = desc.Width;
            info.height = desc.Height;
            info.format = desc.Format;
            info.flags = desc.Flags;
            info.type = UAV;
            info.captureInfo = CaptureInfo::Upscaler;

            Hudfix_Dx12::CheckForHudless(InCmdList, &info,
                                         (D3D12_RESOURCE_STATES) Config::Instance()->OutputResourceBarrier.value_or(
                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                                         true);
        }
        else
        {
            LOG_DEBUG("(FG) running, frame: {0}", feature->FrameCount());
        }
    }
}
