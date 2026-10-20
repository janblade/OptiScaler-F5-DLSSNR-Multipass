#include "pch.h"
#include "../../OptiScaler/dlssnr/DlssNr_SuperiorScaling.h"
#include "../../OptiScaler/shaders/dlssnr/superior/Constants.h"
using namespace DlssNr::Superior;
#include "../../OptiScaler/shaders/dlssnr/DlssNr_Guides.h"

template<class T> struct Setting { T value; T value_or_default() const { return value; } };
struct SavedSettings {
    Setting<uint32_t> DlssNrTransfer {11}, DlssNrInterPassReconstruction {0},
        DlssNrProxyDownscaleFilter {11}, DlssNrGuidedResidualRadius {3}, DlssNrGuidedResidualShaping {0};
    Setting<float> DlssNrGuidedResidualRangeSigma {.2f}, DlssNrGuidedResidualSpatialSigma {.5f},
        DlssNrGuidedResidualGuideStrength {0};
};
void require(bool condition) { if(!condition)throw std::runtime_error("Superior policy check failed"); }
int main() {
    try {
        using namespace DlssNr::SuperiorScaling;
        SavedSettings cfg;
        require(Enabled(cfg) && InterPassMode(cfg)==8 && DownscaleFilter(cfg)==0);
        require(Radius(cfg)==1 && RangeSigma(cfg)==.015f && SpatialSigma(cfg)==3 && GuideStrength(cfg)==1);
        require(ShapingMode(cfg)==2);
        require(High(0)==.8f && Low(0)==.661f && High(1)==.75f && Low(1)==.787f);
        require(High(2)==.95f && Low(2)==.784f && High(99)==High(2));
        require(High(0,2)==.950f && Low(0,2)==.861f);
        require(High(1,2)==.925f && Low(1,2)==.838f);
        require(High(2,2)==.955f && Low(2,2)==.861f);
        require(EffectiveBand(.8f,.5f,1,1)==.8f);
        require(EffectiveBand(.8f,1,1,1)==1 && EffectiveBand(.8f,.5f,.75f,0)==1);
        require(std::abs(EffectiveBand(.8f,.5f,.5f,.5f)-.95f)<1e-6f);
        require(MotionScale(1920,1920,1920)==1920); // native NR, game uses render-size vectors
        require(MotionScale(1920,2496,1920)==2496); // P65 of 3840, matched guides
        require(MotionScale(3840,2496,3840)==2496); // high-resolution motion texture
        require(MotionScale(-1,1274,1440)==-1*(1274.f/1440.f));
        require(MotionScale(1,1274,0)==1); // unspecified game reference
        for(int i=-5;i<30;++i)require(Preset(i)==(i==11?11:1));
        // Native-scale fallback is tested by the host integration contract.
        DlssNrConstants input{};input.Mode=0;input.Width=1920;input.Height=1080;input.DirectResolveFlags=ShaderFlag;
        for(uint32_t pass : {0u,1u})for(uint32_t mode=0;mode<5;++mode) {
            input.Passthrough=pass;input.ReversibleMode=mode;require(InputEncode(input,1920,1080));
        }
        input.Mode=1;require(!InputEncode(input,1920,1080));input.Mode=2;require(!InputEncode(input,1920,1080));input.Mode=0;
        input.ReversibleMode=5;require(!InputEncode(input,1920,1080));input.ReversibleMode=0;
        input.Passthrough=2;require(!InputEncode(input,1920,1080));input.Passthrough=0;
        input.DirectResolveFlags=0;require(!InputEncode(input,1920,1080));input.DirectResolveFlags=ShaderFlag|4;
        require(!InputEncode(input,1920,1080));input.DirectResolveFlags=ShaderFlag;
        require(!InputEncode(input,1921,1080) && !InputEncode(input,1920,1081));
        require(!InputEncode(input,1919,1080) && !InputEncode(input,1920,1079));
        input.Width=0;require(!InputEncode(input,0,1080));input.Width=1920;
        input.Height=0;require(!InputEncode(input,1920,0));input.Width=input.Height=1;require(InputEncode(input,1,1));
        DlssNrConstants exposure{};exposure.Mode=3;exposure.Width=exposure.Height=64;
        exposure.ExposureSourceWidth=3840;exposure.ExposureSourceHeight=2160;exposure.DirectResolveFlags=ShaderFlag;
        require(ExposureStage(exposure,3840,2160)==1);
        require(ExposureStage(exposure,3841,2160)==0 && ExposureStage(exposure,3840,2161)==0);
        exposure.Width=63;require(ExposureStage(exposure,3840,2160)==0);exposure.Width=64;
        exposure.Mode=11;exposure.Width=exposure.Height=1;require(ExposureStage(exposure,64,64)==2);
        require(ExposureStage(exposure,63,64)==0 && ExposureStage(exposure,64,63)==0);
        exposure.Width=2;require(ExposureStage(exposure,64,64)==0);exposure.Width=1;
        exposure.Mode=0;require(ExposureStage(exposure,64,64)==0);exposure.Mode=11;
        exposure.ExposureSourceWidth=0;require(ExposureStage(exposure,64,64)==0);exposure.ExposureSourceWidth=3840;
        exposure.ExposureSourceHeight=0;require(ExposureStage(exposure,64,64)==0);exposure.ExposureSourceHeight=2160;
        exposure.DirectResolveFlags=0;require(ExposureStage(exposure,64,64)==0);exposure.DirectResolveFlags=ShaderFlag|4;
        require(ExposureStage(exposure,64,64)==0);
        DlssNrConstants area{};area.Mode=2;area.Width=2496;area.Height=1404;area.DirectResolveFlags=ShaderFlag;
        require(AreaDownsample(area,3840,2160) && AreaDownsample(area,2496,2160) && AreaDownsample(area,3840,1404));
        require(!AreaDownsample(area,2496,1404) && !AreaDownsample(area,1920,1080));
        require(!AreaDownsample(area,0,2160) && !AreaDownsample(area,3840,0));
        area.Transfer=1;require(!AreaDownsample(area,3840,2160));area.Transfer=0;
        area.Mode=32;require(!AreaDownsample(area,3840,2160));area.Mode=2;
        area.DirectResolveFlags=0;require(!AreaDownsample(area,3840,2160));area.DirectResolveFlags=ShaderFlag|4;
        require(!AreaDownsample(area,3840,2160));area.DirectResolveFlags=ShaderFlag;
        area.Width=0;require(!AreaDownsample(area,3840,2160));area.Width=1;area.Height=0;
        require(!AreaDownsample(area,3840,2160));area.Height=1;require(AreaDownsample(area,1,2));
        DlssNrConstants low{};low.Mode=18;low.Transfer=1;low.Width=312;low.Height=176;low.DirectResolveFlags=ShaderFlag;
        require(LowResidual(low,2496,1404));
        low.Mode=34;require(!LowResidual(low,2496,1404));low.Mode=18;
        low.Transfer=0;require(!LowResidual(low,2496,1404));low.Transfer=1;
        low.Transfer=9;require(!LowResidual(low,2496,1404));low.Transfer=1;
        low.DirectResolveFlags=0;require(!LowResidual(low,2496,1404));low.DirectResolveFlags=ShaderFlag|4;
        require(!LowResidual(low,2496,1404));low.DirectResolveFlags=ShaderFlag;
        require(!LowResidual(low,0,1404) && !LowResidual(low,2496,0));
        low.Width=313;require(LowResidual(low,2497,1404) && !LowResidual(low,2496,1404));
        low.Width=312;require(LowResidual(low,2496,1404)); // 2496 is exactly divisible by eight.
        low.Height=175;require(!LowResidual(low,2496,1404));low.Height=176;
        low.Width=311;require(!LowResidual(low,2496,1404));low.Width=0;require(!LowResidual(low,2496,1404));
        low.Width=low.Height=1;require(LowResidual(low,1,1) && LowResidual(low,8,8) && !LowResidual(low,9,8));
        DlssNr::GuideRegion region {3,5,2560,1440};
        require(ReuseDepth(true,true,1920,1080,1920,1080,region,region));
        require(!ReuseDepth(false,true,1920,1080,1920,1080,region,region));
        require(!ReuseDepth(true,false,1920,1080,1920,1080,region,region));
        require(!ReuseDepth(true,true,1921,1080,1920,1080,region,region));
        require(!ReuseDepth(true,true,1920,1081,1920,1080,region,region));
        require(!ReuseDepth(true,true,0,1080,0,1080,region,region));
        require(!ReuseDepth(true,true,1920,0,1920,0,region,region));
        for(auto field : {&DlssNr::GuideRegion::x,&DlssNr::GuideRegion::y,
                          &DlssNr::GuideRegion::width,&DlssNr::GuideRegion::height}) {
            auto changed=region;++(changed.*field);
            require(!ReuseDepth(true,true,1920,1080,1920,1080,changed,region));
        }
        region.width=0;require(!ReuseDepth(true,true,1920,1080,1920,1080,region,region));
        region.width=2560;region.height=0;
        require(!ReuseDepth(true,true,1920,1080,1920,1080,region,region));
        DlssNrConstants resolve{};resolve.Mode=1;resolve.Transfer=9;resolve.Width=3840;resolve.Height=2160;
        resolve.ResidualHistoryValid=1;resolve.ApplyModel=1;resolve.DirectResolveFlags=ShaderFlag;
        require(NormalResolve(resolve,1920,1080) && NormalResolve(resolve,3840,1080) && NormalResolve(resolve,1920,2160));
        require(!NormalResolve(resolve,3840,2160) && !NormalResolve(resolve,7680,4320));
        require(!NormalResolve(resolve,0,1080) && !NormalResolve(resolve,1920,0) && !NormalResolve(resolve,7680,1080));
        for(auto field : {&DlssNrConstants::DebugView,&DlssNrConstants::CompareMode,&DlssNrConstants::ShowSkinMask}) {
            resolve.*field=1;require(!NormalResolve(resolve,1920,1080));resolve.*field=0;
        }
        resolve.ApplyModel=0;require(!NormalResolve(resolve,1920,1080));resolve.ApplyModel=1;
        resolve.Transfer=8;require(!NormalResolve(resolve,1920,1080));resolve.Transfer=9;
        resolve.Mode=18;require(!NormalResolve(resolve,1920,1080));resolve.Mode=1;
        resolve.ResidualHistoryValid=2;require(!NormalResolve(resolve,1920,1080));resolve.ResidualHistoryValid=1;
        resolve.DirectResolveFlags|=4;require(!NormalResolve(resolve,1920,1080));resolve.DirectResolveFlags=ShaderFlag;
        for(uint32_t mode=0;mode<12;++mode) {
            resolve.ReversibleMode=mode;
            for(uint32_t pass : {0u,1u,2u}) {
                resolve.Passthrough=pass;require(SoftKneeResolve(resolve)==(pass==0 && mode==0));
            }
            resolve.ReversibleMode=mode;resolve.Passthrough=1;require(NormalColorProfile(resolve)==(mode<=4?1:0));
            resolve.Passthrough=0;require(NormalColorProfile(resolve)==(mode==2?2:mode==4?3:0));
            resolve.Passthrough=2;require(NormalColorProfile(resolve)==0);
        }
        for(uint32_t mode : {2u,4u}) {
            resolve.ReversibleMode=mode;resolve.Passthrough=0;resolve.ReplaceDetailStrength=.5f;
            resolve.ModelWorkScale=.5f;require(NormalColorProfile(resolve)==0);
            resolve.Passthrough=1;require(NormalColorProfile(resolve)==0);
            resolve.ModelWorkScale=.999f;require(NormalColorProfile(resolve)==1);
            resolve.ModelWorkScale=1;require(NormalColorProfile(resolve)==1);
            resolve.ModelWorkScale=0;require(NormalColorProfile(resolve)==1);
            resolve.ModelWorkScale=.5f;resolve.ReplaceDetailStrength=0;require(NormalColorProfile(resolve)==1);
        }
        resolve.Width=resolve.Height=1;require(!NormalResolve(resolve,1,1)); // A rounded 1x1 is not reduced.
        for(uint32_t mode=0;mode<=10;++mode) {
            cfg.DlssNrTransfer.value=mode;
            require(!Enabled(cfg) && InterPassMode(cfg)==0 && DownscaleFilter(cfg)==11);
            require(Radius(cfg)==3 && RangeSigma(cfg)==.2f && SpatialSigma(cfg)==.5f && GuideStrength(cfg)==0);
            require(ShapingMode(cfg)==0);
        }
        require(cfg.DlssNrInterPassReconstruction.value==0 && cfg.DlssNrProxyDownscaleFilter.value==11);
        std::cout<<"Superior policy PASS: fixed settings, A/K, all styles, depth reuse/normal PSO guards, native fallback and legacy preservation\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
