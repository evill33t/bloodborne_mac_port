// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "bbport_map_art.h"
#include <algorithm>
#include <cmath>
namespace BbMapView {
// Soft reveal around walked ground/lamps. A 2 m visibility grid avoids a per-pixel search
// through thousands of saved cells. Alpha carries visibility for both map passes.
inline void Fog(const BbMapArt::Chart& chart,std::span<const BbMapArt::Vec3> discs,
                std::vector<std::uint8_t>& rgba) {
    const int width=int(chart.width/chart.ppm/2)+2,height=int(chart.height/chart.ppm/2)+2;
    std::vector<float> grid(size_t(width)*height,.25f);
    for(const auto& disc:discs) {
        float cx=(disc[0]-chart.x0)/2,cy=(chart.z1-disc[1])/2,radius=disc[2]/2;
        for(int y=std::max(0,int(cy-radius-1));y<=std::min(height-1,int(cy+radius+1));++y)
            for(int x=std::max(0,int(cx-radius-1));x<=std::min(width-1,int(cx+radius+1));++x) {
                float visible=std::clamp((radius-std::hypot(x-cx,y-cy))/(radius*.35f),0.f,1.f);
                auto& cell=grid[size_t(y)*width+x];cell=std::max(cell,.25f+.75f*visible);
            }
    }
    for(int y=0;y<chart.height;++y) for(int x=0;x<chart.width;++x) {
        const float gx=x/(2*chart.ppm),gy=y/(2*chart.ppm);
        const int ix=int(gx),iy=int(gy);const float fx=gx-ix,fy=gy-iy;
        const auto at=[&](int a,int b){return grid[size_t(b)*width+a];};
        const float v=(at(ix,iy)*(1-fx)+at(ix+1,iy)*fx)*(1-fy)+(at(ix,iy+1)*(1-fx)+at(ix+1,iy+1)*fx)*fy;
        const size_t pixel=(size_t(y)*chart.width+x)*4;
        rgba[pixel+3]=std::uint8_t(rgba[pixel+3]*v);
    }
}
}
