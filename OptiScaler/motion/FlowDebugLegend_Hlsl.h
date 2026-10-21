#pragma once
// Original compact bitmap font: no external texture, normal path has no legend work.
#define F5LOW_DEBUG_LEGEND_HLSL R"HLSL(
float4 DebugLegend(uint2 pixel,uint mode,float4 image) {
    if(pixel.x<5u || pixel.y<5u || pixel.y>=25u)return image;
    uint view=mode==7u?0u:mode==6u?1u:mode==0u?2u:mode==18u?3u:mode==5u?4u:mode==17u?5u:mode==12u?6u:99u;
    if(view==99u)return image;
    uint2 at=pixel-uint2(7u,7u);
    uint textRow=min(at.y/9u,1u),letter=at.x/6u;
    uint ch=26u, count=0u;
    if(view==0u && textRow==0u){static const uint text[36]={0u,17u,17u,14u,22u,18u,27u,26u,7u,20u,4u,26u,3u,8u,17u,4u,2u,19u,8u,14u,13u,26u,28u,26u,11u,4u,13u,6u,19u,7u,26u,18u,15u,4u,4u,3u};count=36u;ch=text[uint(min(letter,count-1u))];}
    if(view==0u && textRow==1u){static const uint text[33]={21u,8u,18u,8u,1u,11u,4u,26u,12u,14u,19u,8u,14u,13u,26u,28u,26u,15u,8u,23u,4u,11u,18u,26u,15u,4u,17u,26u,5u,17u,0u,12u,4u};count=33u;ch=text[uint(min(letter,count-1u))];}
    if(view==1u && textRow==0u){static const uint text[24]={11u,4u,5u,19u,26u,13u,14u,17u,12u,0u,11u,18u,26u,17u,8u,6u,7u,19u,26u,3u,4u,15u,19u,7u};count=24u;ch=text[uint(min(letter,count-1u))];}
    if(view==1u && textRow==1u){static const uint text[38]={15u,17u,4u,21u,8u,4u,22u,26u,14u,13u,11u,24u,26u,17u,0u,22u,26u,12u,14u,3u,4u,11u,26u,3u,4u,15u,19u,7u,26u,20u,13u,2u,7u,0u,13u,6u,4u,3u};count=38u;ch=text[uint(min(letter,count-1u))];}
    if(view==2u && textRow==0u){static const uint text[25]={5u,8u,13u,0u,11u,26u,5u,11u,14u,22u,27u,26u,7u,20u,4u,26u,3u,8u,17u,4u,2u,19u,8u,14u,13u};count=25u;ch=text[uint(min(letter,count-1u))];}
    if(view==2u && textRow==1u){static const uint text[29]={1u,17u,8u,6u,7u,19u,13u,4u,18u,18u,26u,18u,15u,4u,4u,3u,26u,28u,26u,1u,11u,0u,2u,10u,26u,25u,4u,17u,14u};count=29u;ch=text[uint(min(letter,count-1u))];}
if(view==3u && textRow==0u){static const uint text[38]={6u,17u,4u,4u,13u,26u,18u,19u,17u,14u,13u,6u,26u,28u,26u,14u,17u,0u,13u,6u,4u,26u,22u,4u,0u,10u,26u,14u,17u,26u,17u,4u,9u,4u,2u,19u,4u,3u};count=38u;ch=text[uint(min(letter,count-1u))];}
if(view==3u && textRow==1u){static const uint text[39]={21u,4u,2u,19u,14u,17u,26u,18u,20u,15u,15u,14u,17u,19u,26u,28u,26u,5u,8u,19u,26u,28u,26u,0u,2u,2u,4u,15u,19u,4u,3u,26u,7u,8u,18u,19u,14u,17u,24u};count=39u;ch=text[uint(min(letter,count-1u))];}
    if(view==4u && textRow==0u){static const uint text[35]={17u,4u,3u,26u,19u,4u,23u,19u,20u,17u,4u,26u,28u,26u,6u,17u,4u,4u,13u,26u,15u,7u,14u,19u,14u,12u,4u,19u,17u,8u,2u,26u,5u,8u,19u};count=35u;ch=text[uint(min(letter,count-1u))];}
    if(view==4u && textRow==1u){static const uint text[36]={1u,11u,20u,4u,26u,19u,17u,20u,18u,19u,4u,3u,26u,7u,8u,18u,19u,14u,17u,24u,26u,28u,26u,1u,17u,8u,6u,7u,19u,26u,18u,19u,17u,14u,13u,6u};count=36u;ch=text[uint(min(letter,count-1u))];}
    if(view==5u && textRow==0u){static const uint text[35]={22u,7u,8u,19u,4u,26u,17u,4u,9u,4u,2u,19u,26u,7u,8u,18u,19u,14u,17u,24u,26u,28u,26u,1u,11u,0u,2u,10u,26u,0u,2u,2u,4u,15u,19u};count=35u;ch=text[uint(min(letter,count-1u))];}
    if(view==5u && textRow==1u){static const uint text[30]={6u,17u,4u,24u,26u,15u,0u,17u,19u,8u,0u,11u,26u,28u,26u,2u,20u,19u,18u,26u,17u,4u,9u,4u,2u,19u,26u,0u,11u,11u};count=30u;ch=text[uint(min(letter,count-1u))];}
    if(view==6u && textRow==0u){static const uint text[39]={18u,19u,8u,11u,11u,26u,0u,6u,4u,27u,26u,1u,11u,0u,2u,10u,26u,25u,4u,17u,14u,26u,28u,26u,22u,7u,8u,19u,4u,26u,19u,7u,17u,4u,18u,7u,14u,11u,3u};count=39u;ch=text[uint(min(letter,count-1u))];}
    if(view==6u && textRow==1u){static const uint text[29]={2u,14u,20u,13u,19u,18u,26u,20u,13u,2u,7u,0u,13u,6u,4u,3u,26u,8u,12u,0u,6u,4u,26u,5u,17u,0u,12u,4u,18u};count=29u;ch=text[uint(min(letter,count-1u))];}
    if(pixel.x>=7u+count*6u)return image;
    static const uint2 glyphs[29]={uint2(1663026734u,4u),uint2(3809986095u,3u),uint2(2182120510u,7u),uint2(3810051631u,3u),uint2(3256321087u,7u),uint2(1108837439u,0u),uint2(2736686142u,7u),uint2(1663026737u,4u),uint2(3359772831u,7u),uint2(2459181340u,1u),uint2(1381078321u,4u),uint2(3255862305u,7u),uint2(1662703473u,4u),uint2(1662834289u,4u),uint2(2736309806u,3u),uint2(1108854319u,0u),uint2(2472068654u,5u),uint2(1381484079u,4u),uint2(3775333438u,3u),uint2(138547359u,1u),uint2(2736309809u,3u),uint2(353945137u,1u),uint2(2874852913u,2u),uint2(1654794801u,4u),uint2(138553905u,1u),uint2(3257016863u,7u),uint2(0u,0u),uint2(138416256u,0u),uint2(1109533200u,0u)};
    float3 colour=float3(0.035,0.045,0.055);
    if(pixel.x>=7u && pixel.y>=7u && at.y<16u && at.x%6u<5u && at.y%9u<7u){
        uint bit=(at.y%9u)*5u+at.x%6u;
        uint2 glyph=glyphs[ch];uint word=bit<32u?glyph.x:glyph.y;
        if(((word>>(bit%32u))&1u)!=0u)colour=0.95.xxx;
    }
    return float4(colour,1.0);
}
)HLSL"
