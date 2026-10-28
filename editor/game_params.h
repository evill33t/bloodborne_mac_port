// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace BbMapEditor {
struct BinderFile {
    std::string name;
    std::int32_t id=0;
    std::size_t header=0;
    std::vector<std::uint8_t> bytes;
};
// Strict readers for the retail Bloodborne DFLT/BND4 and 64-bit Unicode PARAM formats.
// No game-specific row layouts are inferred from names or numeric ID ranges.
std::vector<std::uint8_t> UnpackAsset(std::span<const std::uint8_t> bytes);
std::vector<BinderFile> ReadBinder(std::span<const std::uint8_t> bytes);
struct ParamRow {
    std::int32_t id=0;
    std::string name;
    std::size_t offset=0,size=0;
};
struct ParamTable {
    std::string type;
    std::vector<std::uint8_t> bytes;
    std::vector<ParamRow> rows;
    static ParamTable Read(std::span<const std::uint8_t> bytes);
    const ParamRow* Find(std::int32_t id) const;
    std::span<const std::uint8_t> Data(const ParamRow& row) const;
};
struct NpcInfo {std::uint32_t hp=0;std::uint8_t type=0,team=0;};
std::optional<NpcInfo> ReadNpcInfo(const ParamTable& table,std::int32_t id);
class GameParams {
public:
    static GameParams Read(std::span<const std::uint8_t> binder);
    static GameParams Open(const std::filesystem::path& game);
    const ParamTable* Table(const std::string& name) const;
    const std::map<std::string,ParamTable>& Tables() const {return tables;}
    // TalkID references an ESD in the area's talk binder, not a TalkParam row.
    static std::map<std::int32_t,std::string> TalkScripts(const std::filesystem::path& game,const std::string& map);
private:
    std::map<std::string,ParamTable> tables;
};
struct EnemyData;
void ValidateCharacterReferences(const EnemyData& before,const EnemyData& after,const GameParams& params,
                                 const std::map<std::int32_t,std::string>& talk_scripts);
} // namespace BbMapEditor
