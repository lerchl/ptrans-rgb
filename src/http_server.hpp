#pragma once
#include "config.hpp"
#include <functional>
#include <httplib.h>

std::function<void(httplib::Server &, const int)>
make_http_server(const char *app_version,
                 std::atomic<std::shared_ptr<Configuration>> &configuration,
                 std::atomic<std::shared_ptr<const std::string>> &text);
