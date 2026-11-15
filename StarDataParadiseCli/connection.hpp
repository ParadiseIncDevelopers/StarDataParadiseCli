#pragma once

#include "terminal.hpp"

#include <StarDataParadiseDLLler.h>
#include <nlohmann/json.hpp>

#include <filesystem>

namespace cli
{
using Json = nlohmann::json;

struct Profile
{
    std::string host = "127.0.0.1";
    std::string tls_name;
    std::string user;
    std::string ca;
    uint16_t port = 7443;
    uint32_t timeout = 5000;
};

enum class Exit : int
{
    ok = 0, arguments = 2, auth = 3, connection = 4, sql = 5, cancelled = 6, unknown = 7
};

uint32_t number(std::string_view text, uint32_t low, uint32_t high);
Profile read_profile(const std::filesystem::path& path);
Exit exit_for(const sdp_error& error);
sdp_error error_for(sdp_status code, const char* description);

class Connection
{
public:
    Connection() = default;
    ~Connection() { close(); }
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    bool connected() const noexcept { return handle_ != 0; }
    const Json& features() const noexcept { return features_; }
    sdp_error open(const Profile& profile, std::string_view password, const Terminal& terminal);
    sdp_error health(const Terminal& terminal);
    void close() noexcept;

private:
    sdp_connection handle_{};
    Json features_ = Json::object();
};
}
