// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
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
}
