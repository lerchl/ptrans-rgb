#include <condition_variable>
#include <functional>
#include <httplib.h>

#include "lio.hpp"

void from_json(const json &j, DepartureDto &d) {
    d.direction = j.value("direction", std::optional<std::string>{});
    d.countdown = j.at("countdown").get<int>();
    d.real_time = j.at("real_time").get<bool>();
    d.late = j.at("late").get<bool>();
    d.traffic_jam = j.at("traffic_jam").get<bool>();
}

void from_json(const json &j, TripDto &t) {
    t.line = j.at("line").get<std::string>();
    t.direction = j.at("direction").get<std::string>();
    t.foot_minutes_to_station = j.at("foot_minutes_to_station").get<int>();
    t.departures = j.at("departures").get<std::vector<DepartureDto>>();
}

void from_json(const json &j, TimetableDto &tt) {
    tt.trips = j.at("trips").get<std::vector<TripDto>>();
    tt.message = j.value("message", std::optional<std::string>{});
}

TimetableDto parse_timetable(const std::string &body) {
    json j = json::parse(body);
    return j.get<TimetableDto>();
}

void from_json(const json &j, ErrorDto &e) {
    e.message = j.at("message").get<std::string>();
}

ErrorDto parse_error(const std::string &body) {
    json j = json::parse(body);
    return j.get<ErrorDto>();
}

std::function<void(std::string)>
make_timetable_job(std::condition_variable &app_cv, std::mutex &app_mutex,
                   const std::atomic<bool> &app_running,
                   std::atomic<std::shared_ptr<TimetableDto>> &timetable) {
    return
        [&app_cv, &app_mutex, &app_running, &timetable](std::string data_url) {
            std::unique_lock<std::mutex> lock(app_mutex);
            app_cv.wait_for(lock, std::chrono::seconds(10),
                            [&app_running] { return !app_running.load(); });
            httplib::Client cli(data_url);

            while (app_running) {
                auto result = cli.Get("/timetable");
                std::string formatted_time =
                    std::format("{0:%F_%T}", std::chrono::system_clock::now());

                if (!result) {
                    std::cerr << std::format("{} - Could not reach ptrans-data",
                                             formatted_time)
                              << std::endl;
                } else if (result->status == 200) {
                    timetable.store(std::make_shared<TimetableDto>(
                                        parse_timetable(result->body)),
                                    std::memory_order_release);
                } else {
                    ErrorDto error = parse_error(result->body);
                    std::cerr
                        << std::format("{} - {} Could not fetch timetable: {}",
                                       formatted_time, result->status,
                                       error.message)
                        << std::endl;
                }

                app_cv.wait_for(lock, std::chrono::seconds(30),
                                [&app_running] { return !app_running.load(); });
            }
        };
}
