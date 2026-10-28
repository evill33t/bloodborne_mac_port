// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace BbMapEditor {
struct EventArgument {std::string name;int type=0;std::size_t offset=0,width=0;std::map<std::int64_t,std::string> choices;};
struct EventCommand {std::string name;std::vector<EventArgument> arguments;};
class EventDefinitions {
public:
    static EventDefinitions Read(std::span<const std::uint8_t> bytes);
    const EventCommand* Find(int bank,int id) const;
    std::size_t Count() const {return commands.size();}
    std::span<const std::uint8_t> OriginalSource() const {return source;}
private:
    std::vector<std::uint8_t> source;
    std::map<std::pair<int,int>,EventCommand> commands;
};
struct ScriptOperand {
    std::string script;
    std::size_t at=0,width=0;
    int type=0;
    std::uint32_t original=0,value=0;
    bool operator==(const ScriptOperand&) const = default;
};
struct PickupQuest {
    std::string script,treasure;
    std::int32_t event_id=0,trigger_flag=0,completion_flag=0,reward_lot=0;
    bool operator==(const PickupQuest&) const = default;
};
struct EventInstruction {
    int bank=0,id=0;
    std::size_t record=0,args=0,length=0;
};
struct ScriptParameter {std::size_t instruction=0,at=0,length=0,source=0;};
struct ScriptEvent {
    std::int64_t id=0;
    std::string name;
    std::size_t record=0;
    std::vector<EventInstruction> instructions;
    std::vector<ScriptParameter> parameters;
};
class EventFile {
public:
    static EventFile Read(std::span<const std::uint8_t> bytes,std::shared_ptr<const EventDefinitions> definitions={});
    static EventFile Open(const std::filesystem::path& path,std::shared_ptr<const EventDefinitions> definitions,std::span<const std::uint8_t> baseline={});
    std::span<const std::uint8_t> OriginalSource() const {return source;}
    const EventDefinitions* Definitions() const {return definitions.get();}
    const std::vector<ScriptEvent>& Events() const {return events;}
    const EventCommand* Command(const EventInstruction& instruction) const;
    bool Parameterized(std::size_t event,std::size_t instruction,const EventArgument& arg) const;
    ScriptOperand Operand(std::size_t event,std::size_t instruction,std::size_t argument) const;
    ScriptOperand Edit(std::size_t event,std::size_t instruction,std::size_t argument,double value) const;
    static double Number(const ScriptOperand& operand);
    std::vector<std::uint8_t> Save(const std::vector<ScriptOperand>& edits,const std::vector<PickupQuest>& quests) const;
private:
    std::vector<std::uint8_t> source,raw;
    std::vector<ScriptEvent> events;
    std::shared_ptr<const EventDefinitions> definitions;
};
bool ValidEventName(const std::string& name);
} // namespace BbMapEditor
