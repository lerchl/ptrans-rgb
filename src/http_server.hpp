#pragma once
#include "config_manager.hpp"
#include <functional>
#include <httplib.h>

std::function<void(httplib::Server &server, const int port)>
make_http_server(const char *app_version, ConfigManager &config_manager,
                 std::atomic<std::shared_ptr<const std::string>> &text);
