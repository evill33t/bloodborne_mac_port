// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "map_document.h"
#include <functional>
#include <map>
#include <memory>
#include <stop_token>
namespace BbMapEditor {
struct Scene {
    Document document;
    std::shared_ptr<const BbMapArt::AreaMesh> terrain;
    std::map<std::string,std::shared_ptr<const BbMapArt::AreaMesh>> models;
    std::size_t missing_models=0;
};
Scene LoadScene(const std::filesystem::path& game,const std::string& map,
                const std::function<void(std::string)>& progress,std::stop_token stop={});
} // namespace BbMapEditor
