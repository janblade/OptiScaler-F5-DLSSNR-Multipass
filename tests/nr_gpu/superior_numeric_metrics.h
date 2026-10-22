#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

// Numerical comparisons are evidence about these fixtures, not a perceptual guarantee for games.
struct SuperiorNumericMetrics {
    double maxAbs=0,maxNormalized=0,sumSquared=0;
    size_t channels=0,changed=0,nonfinite=0;
    static double value(const std::vector<uint32_t>& words,size_t i,bool half) {
        if(!half) { float v;std::memcpy(&v,&words[i],4);return v; }
        const unsigned bits=(words[i/2]>>(16*(i%2)))&65535,e=(bits>>10)&31,m=bits&1023;
        double v=e==31?(m?std::numeric_limits<double>::quiet_NaN():std::numeric_limits<double>::infinity()):
            e?std::ldexp(double(1024+m),int(e)-25):std::ldexp(double(m),-24);
        return bits&32768?-v:v;
    }
    void compare(const std::vector<uint32_t>& a,const std::vector<uint32_t>& b,bool half) {
        if(a.size()!=b.size())throw std::runtime_error("Numeric image size mismatch");
        for(size_t i=0;i<a.size()*(half?2:1);++i) {
            const double av=value(a,i,half),bv=value(b,i,half);
            if(!std::isfinite(av)||!std::isfinite(bv)) {
                if(!(std::isnan(av)&&std::isnan(bv)) && av!=bv)throw std::runtime_error("Nonfinite classification changed");
                ++nonfinite;continue;
            }
            const double error=std::abs(av-bv);
            if(i%4==3) { if(error!=0)throw std::runtime_error("Alpha changed");continue; }
            maxAbs=std::max(maxAbs,error);
            maxNormalized=std::max(maxNormalized,error/std::max(1.0,std::abs(av)));
            sumSquared+=error*error;++channels;if(error!=0)++changed;
        }
    }
    void print(const char* stage,bool half) const {
        std::cout<<stage<<" FP"<<(half?16:32)<<" max_abs="<<maxAbs<<" max_normalized="<<maxNormalized
            <<" rms="<<std::sqrt(sumSquared/std::max(size_t(1),channels))<<" changed="<<changed<<'/'<<channels
            <<" matched_nonfinite="<<nonfinite<<'\n';
    }
};
