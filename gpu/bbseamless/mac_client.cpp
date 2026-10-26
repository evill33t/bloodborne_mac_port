// SPDX-License-Identifier: GPL-2.0-or-later
#include "client.h"
#include "common/elf_info.h"
#include "common/path_util.h"
#include "core/libraries/libs.h"
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>

namespace {
BbSeamlessHost host{};
std::map<std::string, uintptr_t> imports;
std::map<Common::FS::PathType, std::filesystem::path> paths;
}

extern "C" void bbnet_configure(const char*, const char*, const char*, const char*);
extern "C" void bbnet_register();

namespace Core::Loader {
void SymbolsResolver::AddSymbol(const char* nid, const char* library, const char* module,
                               SymbolType, u64 address) {
    imports[std::string{nid} + "#" + library + "#" + module] = address;
    imports.try_emplace(nid, address);
}
}
extern "C" __attribute__((visibility("default"))) uintptr_t bbnet_resolve(const char* nid) {
    if (!nid) return 0;
    auto found = imports.find(nid);
    if (found == imports.end()) {
        std::string name{nid};
        name.resize(name.find('#') == std::string::npos ? name.size() : name.find('#'));
        found = imports.find(name);
    }
    return found == imports.end() ? 0 : found->second;
}
extern "C" __attribute__((visibility("default"))) int bbseamless_init(const BbSeamlessHost* supplied) {
    if (!supplied || supplied->abi != BBSEAMLESS_ABI_VERSION ||
        supplied->net.abi != BBNET_ABI_VERSION || !supplied->net.thread_spawn ||
        !supplied->net.thread_join || !supplied->thread_attach || !supplied->memory_protection)
        return -1;
    host = *supplied;
    // Only this optional client module chooses our hosted server. Explicit overrides survive.
    const auto default_environment = [](const char* name, const char* value) {
        const char* current = std::getenv(name);
        if (!current || !*current) setenv(name, value, 1);
    };
    default_environment("BB_SHADNET_SERVER", BBSEAMLESS_DEFAULT_LINK);
    default_environment("BB_SHADNET_WEBAPI", BBSEAMLESS_DEFAULT_WEBAPI);
    const char* selected_server = std::getenv("BB_SHADNET_SERVER");
    if (selected_server && (!std::strcmp(selected_server, BBSEAMLESS_DEFAULT_LINK) ||
                            !std::strcmp(selected_server, BBSEAMLESS_DEFAULT_HOST)))
        default_environment("BB_SHADNET_SERVER_KEY", BBSEAMLESS_DEFAULT_SERVER_KEY);
    bbnet_configure(host.net.app0, host.net.serial, host.net.title, host.net.app_ver);
    bbnet_register();
    return 0;
}
extern "C" int32_t runtime_thread_spawn(void** out, void* __attribute__((sysv_abi)) (*entry)(void*),
                                        void* arg, uint64_t stack, const char* name) {
    return host.net.thread_spawn(out, entry, arg, stack, name);
}
extern "C" int32_t runtime_thread_join_spawned(void* thread, void** result) {
    return host.net.thread_join(thread, result);
}
extern "C" void runtime_thread_attach_host(const char* name) { host.thread_attach(name); }
extern "C" int runtime_memory_protection(uintptr_t address, uintptr_t* start,
                                         uintptr_t* end, uint32_t* protection) {
    return host.memory_protection(address, start, end, protection);
}
extern "C" int32_t runtime_trophy_unlock_from_party(int32_t trophy) {
    return host.trophy_unlock ? host.trophy_unlock(trophy) : -1;
}

namespace Common::FS {
const std::filesystem::path& GetUserPath(PathType type) {
    if (paths.empty()) {
        const char* user = std::getenv("BB_USER_DIR");
        const std::filesystem::path root{user && *user ? user : "user"};
        for (int value = 0; value <= static_cast<int>(PathType::CustomModulesDir); ++value)
            paths[static_cast<PathType>(value)] = root;
        paths[PathType::LogDir] = root / "log";
        paths[PathType::CacheDir] = root / "cache";
        paths[PathType::TempDataDir] = root / "temp";
        paths[PathType::GameDataDir] = root / "data";
        for (const auto& [key, path] : paths) {
            std::error_code error;
            std::filesystem::create_directories(path, error);
        }
    }
    return paths.at(type);
}
std::string GetUserPathString(PathType type) { return GetUserPath(type).string(); }
std::string PathToUTF8String(const std::filesystem::path& path) { return path.string(); }
}

namespace Libraries::Kernel {
int* __Error() { static thread_local int error; return &error; }
}
