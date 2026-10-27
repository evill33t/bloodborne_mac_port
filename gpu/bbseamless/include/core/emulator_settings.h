// SPDX-License-Identifier: GPL-2.0-or-later
// Network-only settings adapter for the optional x86-64 client module.
#pragma once
#include <atomic>
#include <cstdlib>
#include <string>
#include "common/types.h"
class EmulatorSettingsImpl {
public:
 static EmulatorSettingsImpl* GetInstance() { static EmulatorSettingsImpl value; return &value; }
 static std::string Text(const char* name,const char* fallback) {
  const char* value=std::getenv(name);return value && *value ? value : fallback;
 }
 bool IsConnectedToNetwork() { return Text("BB_ONLINE","0")=="1"; }
 bool IsShadNetEnabled() { return IsConnectedToNetwork() && !disabled.load(); }
 void SetShadNetSessionDisabled(bool value) { disabled.store(value); }
 std::string GetShadNetServer() { return Text("BB_SHADNET_SERVER","bloodborne.phobos.services:31314"); }
 std::string GetShadNetWebApiServer() { return Text("BB_SHADNET_WEBAPI","http://bloodborne.phobos.services:31315"); }
 std::string GetShadNetServerKey() { return Text("BB_SHADNET_SERVER_KEY",""); }
private:
 std::atomic<bool> disabled{false};
};
#define EmulatorSettings (*EmulatorSettingsImpl::GetInstance())
