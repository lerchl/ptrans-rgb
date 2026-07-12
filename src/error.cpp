#include "error.hpp"

void from_json(const json &j, ErrorDto &e) {
    e.message = j.at("message").get<std::string>();
}

ErrorDto parse_error(const std::string &body) {
    json j = json::parse(body);
    return j.get<ErrorDto>();
}
