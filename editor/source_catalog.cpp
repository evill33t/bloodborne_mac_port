// SPDX-License-Identifier: GPL-2.0-or-later
#include "source_catalog.h"
#include "map_document.h"
#include <algorithm>
#include <charconv>
#include <cctype>
#include <set>
#include <stdexcept>

namespace BbMapEditor {
namespace {
struct Token {std::string value;bool quoted=false;};
std::vector<Token> Tokens(std::string_view text){
    if(text.size()>1048576)throw std::runtime_error("Steam metadata exceeds size limit");std::vector<Token> result;
    for(std::size_t at=0;at<text.size();){
        const auto c=text[at++];if(std::isspace(static_cast<unsigned char>(c)))continue;
        if(c=='/' && at<text.size() && text[at]=='/'){while(at<text.size() && text[at]!='\n')++at;continue;}
        if(c=='{' || c=='}'){result.push_back({std::string(1,c),false});continue;}
        if(c!='"')throw std::runtime_error("Unsupported Steam metadata token");std::string value;bool closed=false;
        while(at<text.size()){
            auto v=text[at++];if(v=='"'){closed=true;break;}
            if(v=='\\'){if(at==text.size())break;v=text[at++];}
            if(!v || value.size()>=16384)throw std::runtime_error("Invalid Steam metadata text");value+=v;
        }
        if(!closed)throw std::runtime_error("Unterminated Steam metadata string");result.push_back({std::move(value),true});
        if(result.size()>100000)throw std::runtime_error("Steam metadata token limit");
    }return result;
}
std::vector<Token> Read(const std::filesystem::path& path){if(std::filesystem::file_size(path)>1048576)throw std::runtime_error("Steam metadata exceeds size limit");const auto bytes=ReadFile(path);return Tokens(std::string_view(reinterpret_cast<const char*>(bytes.data()),bytes.size()));}
struct Game {const char* profile;int app;const char* executable;};
// Executable locations follow Smithbox's SteamGameLocator integration.
constexpr Game Games[]={{"DS1",211420,"DATA/DARKSOULS.exe"},{"DS1R",570940,"DarkSoulsRemastered.exe"},{"DS2",236430,"Game/DarkSoulsII.exe"},{"DS2S",335300,"Game/DarkSoulsII.exe"},{"DS3",374320,"Game/DarkSoulsIII.exe"},{"SDT",814380,"sekiro.exe"},{"ER",1245620,"Game/eldenring.exe"},{"AC6",1888160,"Game/armoredcore6.exe"},{"NR",2622380,"Game/nightreign.exe"}};
}
std::vector<SourceInstall> DiscoverSources(const std::filesystem::path& steam,const std::filesystem::path& bloodborne){
    namespace fs=std::filesystem;std::set<fs::path> libraries;std::vector<SourceInstall> result;
    if(!steam.empty())libraries.insert(fs::weakly_canonical(steam));
    for(const auto& path:{steam/"steamapps/libraryfolders.vdf",steam/"config/libraryfolders.vdf"}){
        try{
            if(steam.empty() || !fs::is_regular_file(path))continue;const auto tokens=Read(path);
            for(std::size_t i=0;i+1<tokens.size();++i)if(tokens[i].quoted && tokens[i+1].quoted){
                const auto& key=tokens[i].value;const auto& value=tokens[i+1].value;
                const bool legacy=!key.empty() && key.find_first_not_of("0123456789")==std::string::npos;
                if((key=="path" || legacy) && fs::path(value).is_absolute() && fs::is_directory(fs::path(value)/"steamapps"))libraries.insert(fs::weakly_canonical(value));++i;
            }
        }catch(const std::exception&){/* Other libraries and manual folder selection remain available. */}
    }
    for(const auto& library:libraries)for(const auto& game:Games){
        try{
            const auto manifest=library/"steamapps"/("appmanifest_"+std::to_string(game.app)+".acf");if(!fs::is_regular_file(manifest))continue;
            const auto tokens=Read(manifest);std::string folder,appid;
            for(std::size_t i=0;i+1<tokens.size();++i)if(tokens[i].quoted && tokens[i+1].quoted){if(tokens[i].value=="installdir")folder=tokens[i+1].value;else if(tokens[i].value=="appid")appid=tokens[i+1].value;++i;}
            if(appid!=std::to_string(game.app) || folder.empty() || folder=="." || folder==".." || folder.find_first_of("/\\")!=std::string::npos)continue;
            const auto executable=library/"steamapps/common"/folder/game.executable;if(!fs::is_regular_file(executable))continue;
            const auto root=fs::weakly_canonical(executable.parent_path());
            if(std::ranges::none_of(result,[&](const auto& installed){return installed.profile==game.profile && installed.root==root;}))result.push_back({game.profile,root});
        }catch(const std::exception&){/* Missing/incomplete or malformed installations are not advertised. */}
    }
    if(!bloodborne.empty() && fs::is_directory(bloodborne/"dvdroot_ps4/map"))result.push_back({"BB",fs::weakly_canonical(bloodborne)});
    std::ranges::sort(result,[](const auto& a,const auto& b){return a.profile==b.profile?a.root<b.root:a.profile<b.profile;});return result;
}
std::vector<SourceMap> ParseSourceMaps(std::string_view text){
    if(text.size()>4*1048576)throw std::runtime_error("Source map catalog exceeds size limit");std::vector<SourceMap> maps;std::set<std::string> ids;
    while(!text.empty()){
        const auto end=text.find('\n');auto line=text.substr(0,end);if(end==text.npos)text={};else text.remove_prefix(end+1);if(line.ends_with('\r'))line.remove_suffix(1);
        if(line.empty())continue;const auto first=line.find('\t'),second=first==line.npos?line.npos:line.find('\t',first+1);
        if(first==line.npos || second==line.npos)throw std::runtime_error("Malformed source map catalog");
        SourceMap map{std::string(line.substr(0,first)),std::string(line.substr(first+1,second-first-1)),0};const auto count=line.substr(second+1);
        const auto parsed=std::from_chars(count.data(),count.data()+count.size(),map.parts);
        if(!ValidMapName(map.id) || map.name.size()>4096 || map.name.find('\0')!=std::string::npos || !ids.insert(map.id).second || parsed.ec!=std::errc{} || parsed.ptr!=count.data()+count.size() || map.parts>1000000 || maps.size()>=10000)throw std::runtime_error("Invalid source map catalog entry");maps.push_back(std::move(map));
    }return maps;
}
}
