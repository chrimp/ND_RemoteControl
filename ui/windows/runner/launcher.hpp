#ifndef LAUNCHER_HPP
#define LAUNCHER_HPP

#include <string>
#include <optional>

struct LauncherConfig {
    std::string ip;
    bool isServer;
    bool isTest;
    bool isCompress;
};

std::optional<LauncherConfig> ShowLauncherDialog();

#endif // LAUNCHER_HPP