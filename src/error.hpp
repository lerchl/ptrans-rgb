#include <nlohmann/json.hpp>
#include <string>

using json = nlohmann::json;

struct ErrorDto {
    std::string message;
};

void from_json(const json &j, ErrorDto &e);
ErrorDto parse_error(const std::string &body);
