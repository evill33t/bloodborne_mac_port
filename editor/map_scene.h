// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "map_document.h"
#include "game_params.h"
#include "collision_scene.h"
#include <functional>
#include <map>
#include <memory>
#include <stop_token>
namespace BbMapEditor {
struct Scene {
    Document document;
    GameParams params;
    std::map<std::int32_t,std::string> talk_scripts;
    std::string param_error,talk_error,event_error,collision_error;
    std::shared_ptr<const CollisionScene> collision;
    std::shared_ptr<const BbMapArt::AreaMesh> terrain;
    std::shared_ptr<const BbMapArt::AreaMesh> enemy_marker;
    std::shared_ptr<const BbMapArt::AreaMesh> region_marker;
    std::map<std::string,std::shared_ptr<const BbMapArt::AreaMesh>> models;
    std::size_t missing_models=0;
};
Scene LoadScene(const std::filesystem::path& game,const std::string& map,
                const std::function<void(std::string)>& progress,std::stop_token stop={});
} // namespace BbMapEditor
