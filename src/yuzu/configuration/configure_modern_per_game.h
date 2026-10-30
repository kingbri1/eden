// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <memory>
#include <vector>

#include "yuzu/configuration/configuration_shared.h"

namespace ConfigurationShared {
class Builder;
}

/// The Modern Console options a game can have of its own (in its properties): each follows the
/// global choice (Emulation > Configure > Controls) unless set for the game
class ConfigureModernPerGame : public ConfigurationShared::Tab {
public:
    explicit ConfigureModernPerGame(std::shared_ptr<std::vector<ConfigurationShared::Tab*>> group,
                                    const ConfigurationShared::Builder& builder,
                                    QWidget* parent = nullptr);
    ~ConfigureModernPerGame() override;

    void ApplyConfiguration() override;
    void SetConfiguration() override;

private:
    std::vector<std::function<void(bool)>> apply_funcs;
};
