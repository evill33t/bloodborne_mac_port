// SPDX-License-Identifier: GPL-2.0-or-later
#include "source_catalog.h"
#include <algorithm>
#include <cassert>
#include <fstream>
#include <iostream>
#include <unistd.h>
using namespace BbMapEditor;
namespace fs=std::filesystem;
void Write(const fs::path& path,const std::string& text){fs::create_directories(path.parent_path());std::ofstream file(path);file<<text;assert(file.good());}
std::string Quote(const std::string& text){std::string escaped="\"";for(char c:text){if(c=='\\' || c=='"')escaped+='\\';escaped+=c;}return escaped+'"';}
void Manifest(const fs::path& steam,int id,const std::string& folder){Write(steam/"steamapps"/("appmanifest_"+std::to_string(id)+".acf"),"\"AppState\" {\"appid\" "+Quote(std::to_string(id))+" \"installdir\" "+Quote(folder)+"}");}
template<class F>void Reject(F f){bool threw=false;try{f();}catch(const std::exception&){threw=true;}assert(threw);}
int main(int argc,char** argv){
    const auto root=fs::temp_directory_path()/("bb-source-catalog-"+std::to_string(getpid()));assert(fs::create_directory(root));
    try{
        const auto primary=root/"Steam",second=root/"Another \"Steam\" Library",bb=root/"Bloodborne";
        fs::create_directories(second/"steamapps");fs::create_directories(bb/"dvdroot_ps4/map");
        fs::create_directory_symlink(second,root/"library-link");
        Write(primary/"steamapps/libraryfolders.vdf","// Steam libraries\n\"libraryfolders\" {\"0\" {\"path\" "+Quote(primary.string())+"} \"1\" {\"path\" "+Quote(second.string())+"} \"2\" {\"path\" "+Quote((root/"library-link").string())+"}}");
        Manifest(primary,374320,"Dark Souls III");Write(primary/"steamapps/common/Dark Souls III/Game/DarkSoulsIII.exe","");
        Manifest(second,570940,"Dark Souls Remastered");Write(second/"steamapps/common/Dark Souls Remastered/DarkSoulsRemastered.exe","");
        Manifest(primary,335300,"Missing install"); // never advertise an incomplete install
        Manifest(primary,1245620,"../Outside");Write(primary/"steamapps/Outside/Game/eldenring.exe","");
        auto found=DiscoverSources(primary,bb);assert(found.size()==3);
        auto get=[&](const std::string& profile){const auto it=std::ranges::find(found,profile,&SourceInstall::profile);assert(it!=found.end());return it->root;};
        assert(get("DS3")==fs::canonical(primary/"steamapps/common/Dark Souls III/Game"));assert(get("DS1R")==fs::canonical(second/"steamapps/common/Dark Souls Remastered"));assert(get("BB")==fs::canonical(bb));
        Write(primary/"steamapps/libraryfolders.vdf","\"libraryfolders\" { \"1\" "+Quote(second.string())+"}");assert(DiscoverSources(primary,bb).size()==3); // legacy library layout
        Write(primary/"steamapps/libraryfolders.vdf","\"unterminated");assert(DiscoverSources(primary,bb).size()==2); // primary + manual BB still work
        Write(primary/"steamapps/appmanifest_374320.acf",std::string(1048577,' '));assert(DiscoverSources(primary,bb).size()==1);
        assert(DiscoverSources({},{}).empty());
        const auto maps=ParseSourceMaps("m10_02_00_00\tFirelink Shrine\t1294\r\nm24_01_00_01\t中央ヤーナム\t1998\n");assert(maps.size()==2 && maps[0].name=="Firelink Shrine" && maps[1].parts==1998);
        for(const auto* bad:{"broken","../source\tBad\t1","m10_02_00_00\tName\t-1","m10_02_00_00\tName\t1000001","m10_02_00_00\tName\t2 extra","m10_02_00_00\tOne\t1\nm10_02_00_00\tTwo\t2"})Reject([&]{ParseSourceMaps(bad);});
        Reject([&]{ParseSourceMaps(std::string(4*1048576+1,' '));});
        std::cout<<"PASS: Steam primary/additional/legacy libraries, escaped paths, incomplete and unsafe manifests, bounded source map catalogs\n";
        if(argc>1){for(const auto& install:DiscoverSources(argv[1],argc>2?argv[2]:""))std::cout<<"DETECTED "<<install.profile<<"\t"<<install.root.string()<<"\n";}
        fs::remove_all(root);
    }catch(...){fs::remove_all(root);throw;}
}
