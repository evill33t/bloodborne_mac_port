// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

class QHttpServer;
namespace HttpGate {
class Gate;
}

namespace WebApiRoutes {

// `gate`: who may ask (http_gate.h); null answers everyone.
void RegisterBloodborneRoutes(QHttpServer& http, bool seamlessCoop,
                              const HttpGate::Gate* gate = nullptr);

} // namespace WebApiRoutes
