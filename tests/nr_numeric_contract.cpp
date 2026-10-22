// Regression for the confidence knee and weak structure eigenvalue.
// Usage: nr_numeric_contract.exe <knee, including 0> [inverse: 0 or 1].
#define main UnusedRegressionMain
#include "nr_optical_flow_gpu.cpp"
#undef main
#include "nr_motion_readback.h"
int main(int argc,char** argv){
    kWidth=641;kHeight=361;Gpu gpu;if(!gpu.Init())return 2;
    OpticalFlowDx12 flow;if(!flow.Init(gpu.device.Get()))return 3;
    flow.Tuning().confidenceKnee=argc>1?float(atof(argv[1])):0.f;
    flow.Tuning().trace=true;
    flow.Tuning().inverseRefinement=argc>2 && atoi(argv[2])!=0;
    unsigned nonfinite=0,negative=0;
    for(int n=0;n<8;++n){
        auto p=gpu.Picture(n*.7f,n*.3f,1,0,0,0,0,0,1);
        if(!flow.Dispatch(gpu.list.Get(),p.Get(),DXGI_FORMAT_R8G8B8A8_UNORM))return 4;
        gpu.Submit();if(!flow.FlowValid())continue;
        auto field=ReadMotionBytes(gpu,flow.MatchedFlow(),8,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        for(size_t i=0;i<field.size();i+=8){uint16_t h[4];memcpy(h,field.data()+i,8);
            for(unsigned j=0;j<3;++j)nonfinite+=(h[j]&0x7c00)==0x7c00;
            negative+=(h[2]&0x8000)!=0 && (h[2]&0x7fff)!=0;
        }
    }
    printf("knee=%.8f nonfinite_xyz=%u negative_structure=%u %s\n",flow.Tuning().confidenceKnee,nonfinite,negative,nonfinite||negative?"FAIL":"PASS");
    return nonfinite||negative?1:0;
}
