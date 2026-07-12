#include <atomic>
#include <condition_variable>
#include <functional>
#include <httplib.h>

#include "error.hpp"
#include "spotify.hpp"

void from_json(const json &j, CurrentlyPlayingDto &d) {
    d.album_cover_url =
        j.value("album_cover_url", std::optional<std::string>{});
    d.is_paused = j.at("is_paused").get<bool>();
}

CurrentlyPlayingDto parse_currently_playing(const std::string &body) {
    json j = json::parse(body);
    return j.get<CurrentlyPlayingDto>();
}

std::function<void(std::string)> make_spotify_job(
    std::condition_variable &app_cv, std::mutex &app_mutex,
    const std::atomic<bool> &app_running,
    std::atomic<std::shared_ptr<CurrentlyPlayingDto>> &currentlyPlaying) {
    return
        [&app_cv, &app_mutex, &app_running,
         &currentlyPlaying](std::string data_url) {
            httplib::Client cli(data_url);

            while (app_running) {
                auto result = cli.Get("/spotify/currentlyPlaying");
                std::string formatted_time =
                    std::format("{0:%F_%T}", std::chrono::system_clock::now());

                if (!result) {
                    std::cerr << std::format("{} - Could not reach ptrans-data",
                                             formatted_time)
                              << std::endl;
                } else if (result->status == 200) {
                    std::cout << result->body << std::endl;
                    currentlyPlaying.store(
                        std::make_shared<CurrentlyPlayingDto>(
                            parse_currently_playing(result->body)),
                        std::memory_order_release);
                } else if (result->status == 404) {
                    std::cout << result->body << std::endl;
                    currentlyPlaying.store(nullptr, std::memory_order_release);
                } else {
                    ErrorDto error = parse_error(result->body);
                    std::cerr
                        << std::format(
                               "{} - {} Could not fetch currently playing: {}",
                               formatted_time, result->status, error.message)
                        << std::endl;
                }

                std::unique_lock<std::mutex> lock(app_mutex);
                app_cv.wait_for(lock, std::chrono::seconds(5),
                                [&app_running] { return !app_running.load(); });
            }
        };
}
