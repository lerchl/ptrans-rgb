#include <condition_variable>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

using json = nlohmann::json;

struct CurrentlyPlayingDto {
    bool is_paused;
    std::optional<std::string> album_cover_url;

    bool operator==(const CurrentlyPlayingDto &) const = default;
};

void from_json(const json &j, CurrentlyPlayingDto &d);

std::function<void(std::string)> make_spotify_job(
    std::condition_variable &app_cv, std::mutex &app_mutex,
    const std::atomic<bool> &app_running,
    std::atomic<std::shared_ptr<std::optional<CurrentlyPlayingDto>>>
        &currently_playing);
