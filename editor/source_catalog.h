// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
namespace BbMapEditor {
struct SourceInstall {std::string profile;std::filesystem::path root;};
struct SourceMap {std::string id,name;std::size_t parts=0;};
// Read-only discovery of Steam manifests, including additional library folders.
std::vector<SourceInstall> DiscoverSources(const std::filesystem::path& steam,const std::filesystem::path& bloodborne={});
std::vector<SourceMap> ParseSourceMaps(std::string_view text);
}
