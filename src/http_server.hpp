#pragma once
#include "config.hpp"
#include <functional>
#include <httplib.h>

std::function<void(httplib::Server &server, const int port)>
make_http_server(const char *app_version,
                 std::atomic<std::shared_ptr<Configuration>> &configuration,
                 std::atomic<std::shared_ptr<const std::string>> &text);
