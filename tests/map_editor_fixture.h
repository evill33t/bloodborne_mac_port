// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "map_document.h"
#include <cstring>

template<class T> void Put(std::vector<std::uint8_t>& b,std::size_t at,T value) {std::memcpy(b.data()+at,&value,sizeof(value));}
void Text(std::vector<std::uint8_t>& b,std::size_t at,const std::string& s) {for(char c:s) {Put<std::uint16_t>(b,at,c);at+=2;}}
std::vector<std::uint8_t> Fixture() {
    std::vector<std::uint8_t> b(0x600,0);
    const std::uint8_t header[]={0x4d,0x53,0x42,0x20,1,0,0,0,0x10,0,0,0,0,0,1,0xff};
    std::memcpy(b.data(),header,16);
    const char* names[]={"MODEL_PARAM_ST","EVENT_PARAM_ST","POINT_PARAM_ST","PARTS_PARAM_ST"};
    for(int i=0;i<4;++i) {
        std::size_t at=0x10+i*0x40;bool record=i==0 || i==3;
        Put<std::uint32_t>(b,at,3);Put<std::uint32_t>(b,at+4,record?2:1);Put<std::int64_t>(b,at+8,0x180+i*0x40);
        Text(b,0x180+i*0x40,names[i]);
        if(record)Put<std::int64_t>(b,at+16,i==0?0x300:0x400);
        Put<std::int64_t>(b,at+16+(record?8:0),i==3?0:at+0x40);
    }
    Put<std::int64_t>(b,0x300,0x40);Text(b,0x340,"o240083");
    Put<std::int64_t>(b,0x408,0xd0);Text(b,0x4d0,"o240083_1025");
    Put<std::uint32_t>(b,0x414,1);Put<std::int32_t>(b,0x41c,0);
    Put<BbMapArt::Vec3>(b,0x428,{1,2,3});Put<BbMapArt::Vec3>(b,0x434,{10,20,30});Put<BbMapArt::Vec3>(b,0x440,{1,1,1});
    Put<std::int64_t>(b,0x4b0,0xf0);Put<std::int32_t>(b,0x4f0,2401000);
    for(std::size_t i=0x500;i<b.size();++i)b[i]=std::uint8_t(i*13); // unknown data must survive
    return b;
}
std::vector<std::uint8_t> EnemyFixture() {
    auto b=Fixture();b.resize(0x1100);
    // Mixed records exercise full-part collision indices and subtype-only patrol indices.
    Put<std::uint32_t>(b,0x14,3);Put<std::int64_t>(b,0x28,0x380);Put<std::int64_t>(b,0x30,0x50);
    Put<std::int64_t>(b,0x380,0x40);Text(b,0x3c0,"c3030");
    Put<std::uint32_t>(b,0x54,3);Put<std::int64_t>(b,0x60,0xb00);Put<std::int64_t>(b,0x68,0xb80);Put<std::int64_t>(b,0x70,0x90);
    Put<std::int64_t>(b,0xb00,0x30);Put<std::uint32_t>(b,0xb0c,1);Text(b,0xb30,"Sound");
    Put<std::int64_t>(b,0xb80,0x30);Put<std::uint32_t>(b,0xb8c,14);Text(b,0xbb0,"Patrol");
    Put<std::int64_t>(b,0xb98,0x40);Put<std::int64_t>(b,0xba0,0x50);
    Put<std::int32_t>(b,0xbc0,-1);Put<std::int32_t>(b,0xbc4,-1);Put<std::int32_t>(b,0xbc8,-1);
    for(int i=0;i<32;++i)Put<std::int16_t>(b,0xbe0+i*2,i==0?0:-1);
    Put<std::uint32_t>(b,0x94,2);Put<std::int64_t>(b,0xa0,0xd00);Put<std::int64_t>(b,0xa8,0xd0);
    Put<std::int64_t>(b,0xd00,0x80);Text(b,0xd80,"Region");
    Put<std::uint32_t>(b,0xd4,4);Put<std::int64_t>(b,0xe8,0x600);Put<std::int64_t>(b,0xf0,0x900);Put<std::int64_t>(b,0xf8,0);
    Put<std::int64_t>(b,0x608,0xd0);Text(b,0x6d0,"c3030_0001");
    Put<std::uint32_t>(b,0x614,2);Put<std::int32_t>(b,0x61c,1);Put<std::int32_t>(b,0x610,14);
    Put<BbMapArt::Vec3>(b,0x628,{5,6,7});Put<BbMapArt::Vec3>(b,0x640,{1,1,1});
    Put<std::int64_t>(b,0x6b0,0x100);Put<std::int64_t>(b,0x6b8,0x120);Put<std::int32_t>(b,0x700,2400200);
    Put<std::int32_t>(b,0x728,303000);Put<std::int32_t>(b,0x72c,303001);Put<std::int32_t>(b,0x730,-1);
    Put<std::int32_t>(b,0x738,-1);Put<std::int32_t>(b,0x73c,2);Put<std::int16_t>(b,0x740,0);
    for(int i=0;i<8;++i)Put<std::int16_t>(b,0x748+i*2,i==0?0:i==1?9:-1);
    Put<std::int32_t>(b,0x758,7000);Put<std::int32_t>(b,0x75c,-1);
    Put<std::int64_t>(b,0x908,0xd0);Put<std::uint32_t>(b,0x914,5);Text(b,0x9d0,"h0000");
    Put<std::int64_t>(b,0x9b0,0x100);Put<std::int32_t>(b,0xa00,2409000);
    Put<std::int64_t>(b,0xd48,0xa0);Put<std::int32_t>(b,0xda0,2409001);
    Put<std::int64_t>(b,0xb18,0x48);Put<std::int32_t>(b,0xb50,2409002);
    return b;
}
BbMapEditor::CharacterImport CharacterFixture(const std::string& model="c6900") {
    BbMapEditor::CharacterImport a;a.model=model;a.native_model="c3030";a.source_game="DS3";a.source_model="c1430";a.report="Fixture conversion";
    a.files["chr/"+model+".chrbnd.dcx"]={1,2,3};a.files["chr/"+model+".anibnd.dcx"]={4,5,6};a.files["action/script/"+model+".hks"]={7,8,9};
    const std::string magic="BBMSRC01";a.preview.assign(magic.begin(),magic.end());return a;
}
