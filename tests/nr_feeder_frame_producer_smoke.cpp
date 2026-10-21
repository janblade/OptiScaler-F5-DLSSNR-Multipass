#include "../OptiScaler/dlssnr/DlssNr_FrameProducer.h"
#include <map>
#include <string>
#include <cstdio>
int main(){
 using M=std::map<std::string,unsigned>;M valid{{"DFC.Feeder.ContractVersion",1},{"DFC.Feeder.ProviderId",0x444c3546u},{"DFC.Feeder.HostMode",0},{"DFC.Feeder.EvaluateCadence",1}};
 auto check=[](const M& v){return DlssNrFrameProducer::IsFeederFrame([&](const char* k,unsigned* out){auto i=v.find(k);if(i==v.end())return false;*out=i->second;return true;});};
 unsigned tests=0;auto require=[&](bool v){++tests;if(!v){printf("FAIL %u\n",tests);return false;}return true;};
 if(!require(check(valid)))return 1;auto host=valid;host["DFC.Feeder.HostMode"]=1;if(!require(check(host)))return 1;
 if(!require(!check(M{})))return 1;
 for(auto& e:valid){auto missing=valid;missing.erase(e.first);if(!require(!check(missing)))return 1;auto invalid=valid;invalid[e.first]=99;if(!require(!check(invalid)))return 1;}
 printf("PASS %u producer contract cases; host0/1 accepted, native/no marker and every incomplete/invalid tuple rejected\n",tests);return 0;
}
