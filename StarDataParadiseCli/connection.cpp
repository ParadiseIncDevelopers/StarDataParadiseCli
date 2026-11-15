#include <winsock2.h>
#include <ws2tcpip.h>

#include "connection.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <fstream>
#include <future>
#include <memory>
#include <vector>

namespace cli
{
sdp_text text(std::string_view value)
{
    return {value.data(), static_cast<uint32_t>(value.size())};
}

std::string_view Buffer::bytes() const
{
    if (!value.data || !value.size) return {};
    return {reinterpret_cast<const char*>(value.data), value.size};
}

Json Buffer::json() const
{
    return Json::parse(bytes());
}

namespace
{
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

class Winsock
{
public:
    Winsock()
    {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data)) throw std::runtime_error("Winsock unavailable");
    }
    ~Winsock() { WSACleanup(); }
};

struct Event
{
    HANDLE handle = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ~Event() { if (handle) CloseHandle(handle); }
};

std::vector<std::string> addresses(std::string_view hostname, DWORD timeout, const Terminal& terminal)
{
    const Winsock sockets;
    const auto name = wide(hostname);
    ADDRINFOEXW hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    ADDRINFOEXW* result{};
    HANDLE cancel{};
    Event done;
    if (!done.handle) throw std::runtime_error("DNS completion event unavailable");
    OVERLAPPED notify{};
    notify.hEvent = done.handle;

    auto status = GetAddrInfoExW(name.c_str(), nullptr, NS_DNS, nullptr, &hints, &result,
        nullptr, &notify, nullptr, &cancel);
    if (status == WSA_IO_PENDING)
    {
        const std::array HANDLEs{terminal.interrupt(), done.handle};
        const auto wait = WaitForMultipleObjects(static_cast<DWORD>(HANDLEs.size()), HANDLEs.data(), FALSE, timeout);
        if (wait != WAIT_OBJECT_0 + 1)
        {
            GetAddrInfoExCancel(&cancel);
            WaitForSingleObject(done.handle, INFINITE);
        }
        status = GetAddrInfoExOverlappedResult(&notify);
    }

    const std::unique_ptr<ADDRINFOEXW, decltype(&FreeAddrInfoExW)> resolved(result, &FreeAddrInfoExW);
    std::vector<std::string> output;
    if (status) return output;
    for (auto address = result; address && output.size() < 16; address = address->ai_next)
    {
        std::array<wchar_t, NI_MAXHOST> numeric{};
        if (GetNameInfoW(address->ai_addr, static_cast<socklen_t>(address->ai_addrlen), numeric.data(),
            static_cast<DWORD>(numeric.size()), nullptr, 0, NI_NUMERICHOST) == 0)
            output.push_back(utf8(numeric.data()));
    }
    return output;
}

std::string profile_value(const Json& value, size_t limit)
{
    if (!value.is_string()) throw std::invalid_argument("Invalid profile string");
    auto text = value.get<std::string>();
    if (text.empty() || text.size() > limit || text.find('\0') != text.npos)
        throw std::invalid_argument("Invalid profile field length");
    return text;
}

uint32_t profile_number(const Json& value, uint32_t low, uint32_t high)
{
    if (!value.is_number_unsigned() || value < low || value > high)
        throw std::invalid_argument("Invalid numeric profile value");
    return value.get<uint32_t>();
}

}

uint32_t number(std::string_view text, uint32_t low, uint32_t high)
{
    uint32_t value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value < low || value > high)
        throw std::invalid_argument("Invalid numeric option");
    return value;
}

Profile read_profile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::invalid_argument("Cannot open connection profile");
    std::string bytes(8193, '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (input.bad() || !input.eof()) throw std::invalid_argument("Cannot read connection profile or profile exceeds 8192 bytes");
    bytes.resize(static_cast<size_t>(input.gcount()));
    if (bytes.find('\0') != bytes.npos) throw std::invalid_argument("Invalid connection profile");

    const auto object = Json::parse(bytes, [](int depth, Json::parse_event_t event, Json&) {
        if ((event == Json::parse_event_t::object_start || event == Json::parse_event_t::array_start) && depth > 1)
            throw std::invalid_argument("Invalid connection profile nesting");
        return true;
    }, false);
    if (!object.is_object()) throw std::invalid_argument("Connection profile must be a JSON object");

    Profile profile;
    for (const auto& [key, value] : object.items())
    {
        if (key == "host") profile.host = profile_value(value, 253);
        else if (key == "port") profile.port = static_cast<uint16_t>(profile_number(value, 1, 65535));
        else if (key == "user") profile.user = profile_value(value, 64);
        else if (key == "tls_name") profile.tls_name = profile_value(value, 253);
        else if (key == "timeout_ms") profile.timeout = profile_number(value, 100, 120000);
        else if (key == "production" && value.is_boolean()) profile.production = value.get<bool>();
        else if (key == "ca_file")
        {
            const auto ca = std::filesystem::path(wide(profile_value(value, 32768)));
            profile.ca = utf8((std::filesystem::absolute(path).parent_path() / ca).wstring());
        }
        else throw std::invalid_argument("Unknown connection profile field");
    }
    return profile;
}

Exit exit_for(const sdp_error& error, bool sql)
{
    if (error.unknown_outcome) return Exit::unknown;
    switch (error.code)
    {
    case SDP_OK: return Exit::ok;
    case SDP_AUTH_FAILED:
    case SDP_PERMISSION_DENIED: return Exit::auth;
    case SDP_INVALID_ARGUMENT: return sql ? Exit::sql : Exit::arguments;
    case SDP_CANCELLED: return Exit::cancelled;
    case SDP_FEATURE_UNSUPPORTED:
    case SDP_INVALID_STATE:
    case SDP_MODULE_UNAVAILABLE:
    case SDP_RESOURCE_LIMIT: return Exit::sql;
    default: return Exit::connection;
    }
}

sdp_error error_for(sdp_status code, const char* description)
{
    sdp_error error{};
    error.code = code;
    const auto message = std::string_view(description).substr(0, sizeof(error.message) - 1);
    message.copy(error.message, message.size());
    return error;
}

sdp_error Connection::open(const Profile& profile, std::string_view password, const Terminal& terminal)
{
    close();
    const auto& name = profile.tls_name.empty() ? profile.host : profile.tls_name;
    constexpr std::string_view host_chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-:%";
    if (profile.host.empty() || profile.host.size() > 253 || profile.host.find_first_not_of(host_chars) != profile.host.npos ||
        name.size() > 253 || name.find_first_not_of(host_chars) != name.npos || profile.user.empty() ||
        profile.user.size() > 64 || profile.ca.empty() || profile.ca.size() > 32768 || password.empty())
        return error_for(SDP_INVALID_ARGUMENT, "Provide --host, --user and --ca or a complete profile");

    const auto deadline = Clock::now() + std::chrono::milliseconds(profile.timeout);
    const auto candidates = addresses(profile.host, profile.timeout, terminal);
    auto error = error_for(SDP_CONNECTION_LOST, "Host could not be resolved");
    for (const auto& address : candidates)
    {
        if (terminal.interrupted()) break;
        const auto time = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (time < 100)
        {
            error = error_for(SDP_TIMEOUT, "Connection timed out");
            break;
        }

        sdp_open_options options{};
        options.struct_size = sizeof(options);
        options.abi_version = SDP_ABI_VERSION;
        options.address = text(address);
        options.server_name = text(name);
        options.ca_file = text(profile.ca);
        options.username = text(profile.user);
        options.password = text(password);
        options.port = profile.port;
        options.timeout_ms = static_cast<uint32_t>(time);

        const auto status = sdp_open(&options, &handle_, &error);
        if (status != SDP_CONNECTION_LOST) break;
    }

    if (terminal.interrupted())
    {
        close();
        return error_for(SDP_CANCELLED, "Connection cancelled");
    }

    if (!connected()) return error;
    Buffer reply;
    if (sdp_get_capabilities(handle_, &reply.value, &error) == SDP_OK)
    {
        const auto* data = reinterpret_cast<const char*>(reply.value.data);
        if (data && reply.value.size)
        {
            auto features = Json::parse(data, data + reply.value.size, nullptr, false);
            if (features.is_object() && !features.empty() &&
                std::ranges::all_of(features, [](const Json& feature) { return feature.is_boolean(); }))
            {
                features_ = std::move(features);
                Buffer identity;
                if (sdp_get_session_info(handle_, &identity.value, &error) == SDP_OK)
                {
                    session_ = Json::parse(identity.bytes(), nullptr, false);
                    if (session_.is_object() && session_.contains("session_id")) return {};
                    error = error_for(SDP_MALFORMED_MESSAGE, "Invalid session info received");
                }
                close();
                return error;
            }
        }
        error = error_for(SDP_MALFORMED_MESSAGE, "Invalid capabilities received");
    }
    close();
    return error;
}

sdp_error Connection::health(const Terminal& terminal)
{
    return request(terminal, sdp_health);
}

sdp_error Connection::request(const Terminal& terminal, const Request& operation)
{
    if (!connected()) return error_for(SDP_CONNECTION_LOST, "Disconnected");
    if (terminal.interrupted()) return error_for(SDP_CANCELLED, "Operation cancelled");
    const auto handle = handle_;
    auto pending = std::async(std::launch::async, [&operation, handle] {
        sdp_error error{};
        error.code = operation(handle, &error);
        return error;
    });
    while (pending.wait_for(25ms) == std::future_status::timeout)
    {
        if (terminal.interrupted()) sdp_cancel(handle, nullptr);
    }
    auto error = pending.get();
    if (error.unknown_outcome || error.code == SDP_CANCELLED || error.code == SDP_CONNECTION_LOST ||
        error.code == SDP_TIMEOUT || error.code == SDP_MALFORMED_MESSAGE) close();
    return error;
}

void Connection::close() noexcept
{
    if (handle_)
    {
        sdp_close(handle_, nullptr);
        handle_ = 0;
    }
    session_.clear();
    features_.clear();
}
}
