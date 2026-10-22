#include "../OptiScaler/resource_tracking/GenericDepth_Select.h"
#include <cstdio>
int main(){using namespace GenericDepthSelect;Candidate scene{1,2048,2048,130,13866,74,0,0,-1,true},small{2,1024,1024,130,4420,449,0,0,-1,false};Rules r;if(Eligible(scene,3413,1920,r))return 1;r.selectedWidth=r.selectedHeight=2048;if(!Eligible(scene,3413,1920,r)||Eligible(small,3413,1920,r))return 2;scene.drawcalls=0;if(Eligible(scene,3413,1920,r))return 3;puts("PASS explicit square scene size bypasses aspect only, rejects other size and unused buffers; automatic rules unchanged");}
