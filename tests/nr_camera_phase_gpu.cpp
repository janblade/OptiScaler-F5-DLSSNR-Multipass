// Synthetic start/stop/reversal and static HUD, using production kernels.
#define main UnusedFlowRegressionMain
#include "nr_optical_flow_gpu.cpp"
#undef main
#include "nr_motion_readback.h"
#include "../OptiScaler/native/F5LowShaderBytecode.h"
int main(int argc,char** argv){
    setvbuf(stdout,nullptr,_IONBF,0);kWidth=641;kHeight=361;Gpu g;if(!g.Init())return 2;
    puts("scene,frame,region,samples,EPE,magnitude,opposed,missing");
    for(int scene:{0,3})for(bool hud:{false,true}){
        OpticalFlowDx12 f;if(!f.Init(g.device.Get()))return 3;
        float px=0,py=0;
        for(int n=0;n<28;++n){
            float vx=n<4?0:n<12?.7f:n<17?0:n<25?-.7f:0,vy=vx*3.f/7.f;
            px+=vx;py+=vy;auto color=g.Picture(px,py,1,0,n,0,0,0,scene,hud?1:0);
            if(!f.Dispatch(g.list.Get(),color.Get(),DXGI_FORMAT_R8G8B8A8_UNORM))return 4;g.Submit();if(!f.FlowValid())continue;
            auto field=ReadFlow(g,f.Flow(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            if(true) {
                auto bytes=ReadMotionBytes(g,f.CameraMotionModel(),8,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                uint16_t raw[16];memcpy(raw,bytes.data(),32);float model[16];
                for(unsigned i=0;i<16;++i)model[i]=Half(raw[i]);
                printf("model,%s%s,%d,valid=%.0f,quality=%.6f,sky_x=%.6f,sky_y=%.6f,rms=%.6f,inliers=%.6f\n",scene==3?"weak":"textured",hud?"+hud":"",n,model[3],model[7],model[0]-.6f*model[2],model[4]-.6f*model[6],model[8],model[9]);
            }

            auto d=f.Flow()->GetDesc();double sum[3]{},mag[3]{};unsigned count[3]{},opposed[3]{},missing[3]{};
            for(unsigned y=20;y+20<d.Height;++y)for(unsigned x=20;x+20<d.Width;++x){
                float sx=(x+.5f)*2,sy=(y+.5f)*2;bool in=hud&&InHud(sx,sy);
                if(in&&!InHudFill(sx-4,sy-4))continue;
                if(in&&!InHudFill(sx+4,sy+4))continue;
                unsigned r=in?2:sy<.3f*kHeight?0:sy>.55f*kHeight?1:3;if(r==3)continue;
                double tx=in?0:-vx,ty=in?0:-vy;auto p=(size_t(y)*d.Width+x)*2;
                double a=field[p],b=field[p+1];sum[r]+=hypot(a-tx,b-ty);mag[r]+=hypot(a,b);++count[r];
                opposed[r]+=a*tx+b*ty<0;missing[r]+=hypot(tx,ty)>.1&&hypot(a,b)<.05;
            }
            for(unsigned r=0;r<3;++r)if(count[r])printf("%s%s,%d,%s,%u,%.7f,%.7f,%.7f,%.7f\n",scene==3?"weak":"textured",hud?"+hud":"",n,r==0?"upper":r==1?"lower":"hud",count[r],sum[r]/count[r],mag[r]/count[r],double(opposed[r])/count[r],double(missing[r])/count[r]);
        }
    }
    return FAILED(g.device->GetDeviceRemovedReason())?5:0;
}
