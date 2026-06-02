#pragma once
#include "config.hpp"
#include <atomic>
#include <memory>
#include <mutex>
#include <string>

class ConfigManager {
  public:
    explicit ConfigManager(std::string path);

    bool patch(const PatchConfigurationDto &patch);

    std::shared_ptr<const Configuration> get() const;

  private:
    std::mutex write_mutex_;
    std::string path_;
    std::atomic<std::shared_ptr<Configuration>> config_;

    Configuration load() const;
    void save(const Configuration &c) const;
};
