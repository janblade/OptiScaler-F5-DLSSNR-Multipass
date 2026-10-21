#include <d3dcompiler.h>
#include <wrl/client.h>
#include <fstream>
#include <cstdio>
#include <cstring>
#include "../OptiScaler/native/DepthCalibration_Hlsl.h"
using Microsoft::WRL::ComPtr;
int main(int argc,char** argv){ComPtr<ID3DBlob> b,e;auto h=D3DCompile(DepthCalibrationHlsl::source,strlen(DepthCalibrationHlsl::source),"DepthCalibration",nullptr,nullptr,"Align","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&b,&e);if(FAILED(h)){if(e)puts((char*)e->GetBufferPointer());return 1;}if(argc!=2)return 2;std::ofstream f(argv[1]);f<<"#pragma once\nnamespace DepthCalibrationBytecode { inline constexpr unsigned char code[]={\n";auto p=(unsigned char*)b->GetBufferPointer();for(size_t i=0;i<b->GetBufferSize();++i){char n[8];sprintf_s(n,"0x%02x,",p[i]);f<<n;if(i%24==23)f<<"\n";}f<<"\n}; }\n";printf("PASS: %zu bytes compiled ahead of time\n",b->GetBufferSize());}
