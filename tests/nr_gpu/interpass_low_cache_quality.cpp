// Same 36 weights: audit coordinate rounding, signed DC, HDR/NaN, partial groups and alpha.
// Stress bounds are numerical contracts, not a perceptual quality verdict.
#include "pch.h"
#include "interpass_fixture.h"
#include <cmath>

float value(const std::vector<uint32_t>& words,size_t index,bool half) {
 if(!half){float v;std::memcpy(&v,&words[index],4);return v;}
 unsigned bits=(words[index/2]>>(16*(index%2)))&65535,e=(bits>>10)&31,m=bits&1023;
 float v=e ? std::ldexp(float(1024+m),int(e)-25) : std::ldexp(float(m),-24);
 if(e==31) throw std::runtime_error("Nonfinite half");
 return bits&32768?-v:v;
}
int main(int argc,char**argv) {
 try {
  if(argc<3 || argc>4)throw std::runtime_error("baseline candidate");
  Runner r(true);auto a=r.pipeline(argv[1],"dlssnr_interpass_low36");
  auto b=r.pipeline(argv[2],"dlssnr_interpass_low36_cached");
  auto cross=r.pipeline(argv[1],"dlssnr_interpass_cross5");
  const bool full=argc==4 && std::string(argv[3])=="--4k";
  unsigned checks=0;double lowMax=0,chainMax=0,lowRms=0,chainRms=0,lowMaxFP32=0,chainMaxFP32=0;size_t n=0,nChain=0;
  const std::vector<std::pair<UINT,UINT>> sizes=full ? std::vector<std::pair<UINT,UINT>>{{3840,2160}} :
    std::vector<std::pair<UINT,UINT>>{{1,1},{17,11},{65,37},{127,73},{257,143}};
  const std::vector<int> scales=full ? std::vector<int>{50,59,65} : std::vector<int>{33,40,41,42,49,50,59,65,74,84,85,90};
  for(auto dims:sizes)
  for(int scale:scales)
  for(int stress=0;stress<4;++stress)for(bool half:{false,true}) {
   UINT ww=std::max(1u,UINT(std::lround(dims.first*scale/100.0f))),wh=std::max(1u,UINT(std::lround(dims.second*scale/100.0f)));
   UINT lw=(ww+7)/8,lh=(wh+7)/8;
   DlssNrConstants c{};c.Mode=34;c.Width=lw;c.Height=lh;
   auto old=r.run(a.Get(),dims.first,dims.second,lw,lh,c,stress,half,8,ww,wh);
   auto now=r.run(b.Get(),dims.first,dims.second,lw,lh,c,stress,half,8,ww,wh);
   for(size_t i=0;i<size_t(lw)*lh*4;++i){double av=value(old,i,half),bv=value(now,i,half),diff=std::abs(av-bv);
    if(!std::isfinite(av)||!std::isfinite(bv))throw std::runtime_error("Invalid low");
    if(!half)lowMaxFP32=std::max(lowMaxFP32,diff);lowMax=std::max(lowMax,diff);lowRms+=diff*diff;++n;}
   c={};c.Mode=36;c.Width=ww;c.Height=wh;c.ResidualScale=3;c.ResidualBlend=.015f;c.ResidualConfidenceSensitivity=1;
   c.GuideWidth=1;c.MvScaleX=1.13f;c.MvScaleY=.71f;
   auto before=r.run(cross.Get(),dims.first,dims.second,ww,wh,c,stress,half,8,0,0,nullptr,0,&old,lw,lh);
   auto after=r.run(cross.Get(),dims.first,dims.second,ww,wh,c,stress,half,8,0,0,nullptr,0,&now,lw,lh);
   for(size_t i=0;i<size_t(ww)*wh*4;++i){double av=value(before,i,half),bv=value(after,i,half),diff=std::abs(av-bv);
    if(!std::isfinite(av)||!std::isfinite(bv))throw std::runtime_error("Invalid chain");
    if(i%4==3 && diff!=0)throw std::runtime_error("Alpha changed");
    if(!half)chainMaxFP32=std::max(chainMaxFP32,diff);chainMax=std::max(chainMax,diff);chainRms+=diff*diff;++nChain;}
   c={};c.Mode=34;c.Width=lw;c.Height=lh;
   for(int sign : {-1,1}) {
    const auto dc=r.run(b.Get(),dims.first,dims.second,lw,lh,c,sign==-1?1:0,false,8,ww,wh,nullptr,1);
    for(size_t i=0;i<size_t(lw)*lh*4;++i)
     if(i%4!=3 && std::abs(value(dc,i,false)-sign*.08f)>1e-6f)
      throw std::runtime_error("Signed DC or edge normalization changed");
   }
   ++checks;
  }
  if(lowMaxFP32>4e-5 || chainMaxFP32>2e-5 || chainMax>0.02)
   throw std::runtime_error("Coordinate rounding exceeds recorded stress bounds");
  std::cout<<"PASS comparisons="<<checks<<" lowMax="<<lowMax<<" lowRms="<<std::sqrt(lowRms/n)<<" chainMax="<<chainMax<<" chainRms="<<std::sqrt(chainRms/nChain)<<" lowMaxFP32="<<lowMaxFP32<<" chainMaxFP32="<<chainMaxFP32<<"\n";
 }catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 1;}
}
