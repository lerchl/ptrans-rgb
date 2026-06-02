#include "http_server.hpp"
#include "config_manager.hpp"

struct TextDto {
    std::string text;
};

inline void from_json(const json &j, TextDto &t) {
    t.text = j.at("text").get<std::string>();
}

inline void to_json(json &j, const TextDto &t) { j = json{{"text", t.text}}; }

std::function<void(httplib::Server &, const int)>
make_http_server(const char *app_version, ConfigManager &config_manager,
                 std::atomic<std::shared_ptr<const std::string>> &text) {
    return [app_version, &config_manager, &text](httplib::Server &server,
                                                 const int port) {
        server.set_default_headers({
            {"Access-Control-Allow-Origin", "*"},
            {"Access-Control-Allow-Methods",
             "GET, POST, PUT, PATCH, DELETE, OPTIONS"},
            {"Access-Control-Allow-Headers", "Content-Type"},
        });

        server.Options(".*", [](const httplib::Request &,
                                httplib::Response &res) { res.status = 204; });

        server.Get("/version", [app_version](const httplib::Request &,
                                             httplib::Response &res) {
            json j = {{"version", app_version}};
            res.status = 200;
            res.set_content(j.dump(), "application/json");
        });

        server.Get("/configuration", [&config_manager](const httplib::Request &,
                                                       httplib::Response &res) {
            auto config = config_manager.get();
            json j = *config;
            res.status = 200;
            res.set_content(j.dump(), "application/json");
        });

        server.Patch("/configuration", [&config_manager](
                                           const httplib::Request &req,
                                           httplib::Response &res) {
            try {
                auto patch = json::parse(req.body).get<PatchConfigurationDto>();

                if (!config_manager.patch(patch)) {
                    res.status = 400;
                    return;
                }

                res.status = 204;
            } catch (const json::parse_error &e) {
                res.status = 400;
            }
        });

        server.Get("/text",
                   [&text](const httplib::Request &, httplib::Response &res) {
                       if (auto t = text.load(std::memory_order_acquire)) {
                           TextDto dto{.text = *t};
                           json j = dto;
                           res.status = 200;
                           res.set_content(j.dump(), "application/json");
                       } else {
                           res.status = 204;
                       }
                   });

        server.Post("/text", [&text](const httplib::Request &req,
                                     httplib::Response &res) {
            try {
                auto new_text = std::make_shared<std::string>(
                    json::parse(req.body).get<TextDto>().text);
                text.store(new_text, std::memory_order_release);
                res.status = 204;
            } catch (const json::parse_error &e) {
                res.status = 400;
            }
        });

        server.listen("0.0.0.0", port);
    };
}
