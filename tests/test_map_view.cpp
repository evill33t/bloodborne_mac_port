// SPDX-License-Identifier: GPL-2.0-or-later
#include "gpu/shim/bbport_map_view.h"
#include "gpu/shim/bbport_map_art.h"
#include "gpu/shim/bbport_map_fog.h"
#include <cassert>
#include <cstdio>
int main() {
    for(float yaw:{0.f,.7f,1.6f,2.8f}) for(float pitch:{.35f,.9f,1.57f}) {
        BbMapView::Camera camera{500,10,300,1,yaw,pitch};
        camera.zoom=BbMapView::FitZoom(camera,-900,-800,1900,1400,1100,700,-300,900);
        for(float x:{-900.f,1900.f}) for(float z:{-800.f,1400.f}) for(float y:{-300.f,900.f}) {
            auto at=camera.Project(x,y,z);assert(std::abs(at[0])<=550 && std::abs(at[1])<=350);
        }
    }

    // Pings invert marker placement at the player's floor, including negative coordinates,
    // tilted views, rotation and panning. Height affects only the tilted view.
    for(float yaw:{-2.f,0.f,1.57f}) for(float pitch:{.35f,.9f,1.5707963268f}) {
        BbMapView::Camera c{-219,-54,170,3,yaw,pitch};
        for(auto p:{std::array<float,2>{-200,180},std::array<float,2>{30,-60}}) {
            const auto screen=c.Project(p[0],c.y,p[1]);const auto back=c.Ground(screen[0],screen[1]);
            assert(std::abs(p[0]-back[0])<.001f && std::abs(p[1]-back[1])<.001f);
        }
        const auto ground=c.Project(c.x,c.y,c.z),above=c.Project(c.x,c.y+10,c.z);
        assert(ground[0]==0 && ground[1]==0);
        assert(std::abs(above[1]+30*std::cos(pitch))<.001f);
    }
    // The live mesh cache preserves topology and colors and rejects damaged indices/data.
    BbMapArt::MeshBuilder builder(.1f);
    const float triangle[]={-10,2,-3,5,2,-3,-10,2,6};
    const uint32_t colors[]={0xff102030,0xff405060,0xff708090};
    builder.Add(triangle,colors);builder.Add(triangle,colors);
    const auto mesh=builder.Finish();assert(mesh.indices.size()==3 && mesh.positions.size()==9);
    auto saved=BbMapArt::SaveMesh(mesh);auto back=BbMapArt::LoadMesh(saved);
    assert(back && *back==mesh);
    saved.resize(saved.size()/2);assert(!BbMapArt::LoadMesh(saved));
    // Negative-world reveal centered on the player, soft edge, and preserved transparent void.
    BbMapArt::Chart chart;chart.width=chart.height=100;chart.x0=-50;chart.z1=50;chart.ppm=1;
    chart.rgba.assign(100*100*4,255);chart.rgba[3]=0;
    auto pixels=chart.rgba;
    const BbMapArt::Vec3 disc{0,0,40};BbMapView::Fog(chart,std::span(&disc,1),pixels);
    assert(pixels[(50*100+50)*4+3]==255);
    assert(pixels[(50*100+99)*4+3]==63);
    assert(pixels[(50*100+85)*4+3]>63 && pixels[(50*100+85)*4+3]<255);
    assert(pixels[0]==255 && pixels[3]==0);
    std::puts("map-view: projection, picking and mesh cache passed");
}
