#include "config_manager.hpp"
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

const Configuration default_configuration =
    Configuration{.mode = PTRANS,
                  .brightness = 80,
                  .blackout_window = {.start = {.hour = 0, .minute = 0},
                                      .end = {.hour = 0, .minute = 0},
                                      .override = false},
                  .colors = {.fg_default = {.r = 100, .g = 0, .b = 255},
                             .fg_late = {.r = 255, .g = 0, .b = 0},
                             .fg_traffic = {.r = 255, .g = 100, .b = 0},
                             .fg_punctual = {.r = 0, .g = 255, .b = 0}}};

ConfigManager::ConfigManager(std::string path) : path_(std::move(path)) {
    config_.store(std::make_shared<Configuration>(load()));
}

std::shared_ptr<const Configuration> ConfigManager::get() const {
    return config_.load();
}

bool ConfigManager::patch(const PatchConfigurationDto &patch) {
    std::lock_guard<std::mutex> lock(write_mutex_);

    auto current = config_.load();
    auto next = std::make_shared<Configuration>(*current);

    if (patch.brightness) {
        if (*patch.brightness < 0 || *patch.brightness > 100) {
            return false;
        }
        next->brightness = *patch.brightness;
    }

    if (patch.mode) {
        // TODO: validation
        next->mode = *patch.mode;
    }

    if (patch.blackout_window) {
        // TODO: validation
        next->blackout_window = *patch.blackout_window;
    }

    if (patch.colors) {
        // TODO: validation
        next->colors = *patch.colors;
    }

    config_.store(next);
    save(*next);

    return true;
}

Configuration ConfigManager::load() const {
    if (!std::filesystem::is_regular_file(path_)) {
        return default_configuration;
    }

    std::ifstream f(path_);
    if (!f) {
        return default_configuration;
    }

    json j;
    f >> j;

    return j.get<Configuration>();
}

void ConfigManager::save(const Configuration &c) const {
    json j = c;

    std::string tmp = path_ + ".tmp";

    {
        std::ofstream f(tmp);
        f << j.dump(4);
        f.flush();
    }

    std::filesystem::rename(tmp, path_);
}
