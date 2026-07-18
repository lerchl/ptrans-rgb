#include "config.hpp"
#include <condition_variable>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

using json = nlohmann::json;

struct DepartureDto {
    std::optional<std::string> direction;
    int countdown;
    bool real_time;
    bool late;
    bool traffic_jam;

    bool operator==(const DepartureDto &) const = default;
};

void from_json(const json &j, DepartureDto &d);

struct TripDto {
    std::string line;
    std::string direction;
    int foot_minutes_to_station;
    std::vector<DepartureDto> departures;

    bool operator==(const TripDto &) const = default;
};

void from_json(const json &j, TripDto &t);

struct TimetableDto {
    std::vector<TripDto> trips;
    std::optional<std::string> message;

    bool operator==(const TimetableDto &) const = default;
};

void from_json(const json &j, TimetableDto &tt);
TimetableDto parse_timetable(const std::string &body);

std::function<void(std::string)>
make_timetable_job(std::condition_variable &app_cv, std::mutex &app_mutex,
                   const std::atomic<bool> &app_running,
                   std::atomic<std::shared_ptr<TimetableDto>> &timetable);
