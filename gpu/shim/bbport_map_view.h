// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <algorithm>
#include <cmath>
namespace BbMapView {
// Shared by marker placement and mouse picking. Screen offsets are in logical pixels, +y down.
struct Camera {
    float x=0,y=0,z=0,zoom=1,yaw=0,pitch=1.5707963268f;
    std::array<float,2> Project(float wx,float wy,float wz) const {
        float dx=wx-x,dz=wz-z;
        return {(dx*std::cos(yaw)-dz*std::sin(yaw))*zoom,
            -((dx*std::sin(yaw)+dz*std::cos(yaw))*std::sin(pitch)+(wy-y)*std::cos(pitch))*zoom};
    }
    std::array<float,2> Ground(float sx,float sy) const {
        float u=sx/zoom,v=-sy/(zoom*std::sin(pitch));
        return {x+u*std::cos(yaw)+v*std::sin(yaw),z-u*std::sin(yaw)+v*std::cos(yaw)};
    }
};
// Fit a world-space atlas rectangle in the current projection, including rotated 3D views.
inline float FitZoom(const Camera& camera,float x0,float z0,float x1,float z1,float width,float height,float y0=NAN,float y1=NAN) {
    auto unit=camera;unit.zoom=1;
    if(std::isnan(y0)) y0=camera.y;if(std::isnan(y1)) y1=camera.y;
    float horizontal=1,vertical=1;
    for(float x:{x0,x1}) for(float z:{z0,z1}) for(float y:{y0,y1}) {
        auto p=unit.Project(x,y,z);
        horizontal=std::max(horizontal,std::abs(p[0]));vertical=std::max(vertical,std::abs(p[1]));
    }
    return .9f*std::min(width/(2*horizontal),height/(2*vertical));
}
}
