#include "connection.hpp"

#include <iostream>
#include <optional>

namespace
{
using namespace cli;
using namespace std::string_view_literals;

struct Arguments
{
    Profile profile;
    Color color = Color::automatic;
    bool status{};
    bool json{};
    bool password_stdin{};
    bool connect{};
};

void help()
{
    std::cout << "Usage: StarDataParadiseCLI [--help | --version]\n"
                 "       StarDataParadiseCLI [--profile FILE] [--host HOST] [--port N] [--user USER]\n"
                 "          [--ca PEM] [--tls-name NAME] [--timeout-ms N] [--password-stdin]\n"
                 "          [--status | --json] [--color auto|always|never]\n"
                 "Commands: \\connect [PROFILE], \\status, \\capabilities, \\help, \\quit\n";
}

Color color_mode(std::string_view value)
{
    if (value == "auto") return Color::automatic;
    if (value == "always") return Color::always;
    if (value == "never") return Color::never;
    throw std::invalid_argument("Invalid --color value");
}

Arguments arguments(int count, wchar_t** values)
{
    Arguments args;
    std::optional<std::filesystem::path> path;
    std::optional<std::string> host, user, ca, tls_name;
    std::optional<uint32_t> timeout, port;
    for (int i = 1; i < count; ++i)
    {
        const std::wstring_view option(values[i]);
        if (option == L"--status") args.status = args.connect = true;
        else if (option == L"--json") args.json = args.status = args.connect = true;
        else if (option == L"--password-stdin") args.password_stdin = true;
        else
        {
            if (i + 1 >= count) throw std::invalid_argument("Missing option value");
            const std::wstring_view value(values[++i]);
            if (option == L"--color")
            {
                args.color = color_mode(utf8(value));
                continue;
            }
            args.connect = true;
            if (option == L"--profile") path = std::filesystem::path(value);
            else if (option == L"--host") host = utf8(value);
            else if (option == L"--port") port = number(utf8(value), 1, 65535);
            else if (option == L"--user") user = utf8(value);
            else if (option == L"--ca") ca = utf8(value);
            else if (option == L"--tls-name") tls_name = utf8(value);
            else if (option == L"--timeout-ms") timeout = number(utf8(value), 100, 120000);
            else throw std::invalid_argument("Unknown option");
        }
    }
    if (path) args.profile = read_profile(*path);
    if (host) args.profile.host = std::move(*host);
    if (user) args.profile.user = std::move(*user);
    if (ca) args.profile.ca = std::move(*ca);
    if (tls_name) args.profile.tls_name = std::move(*tls_name);
    if (port) args.profile.port = static_cast<uint16_t>(*port);
    if (timeout) args.profile.timeout = *timeout;
    return args;
}

sdp_error connect(Connection& connection, const Arguments& args, const Terminal& terminal)
{
    connection.close();
    if (args.profile.user.empty() || args.profile.ca.empty())
        return error_for(SDP_INVALID_ARGUMENT, "A user and CA PEM are required");
    if (args.password_stdin && terminal.console_input())
        return error_for(SDP_INVALID_ARGUMENT, "--password-stdin requires redirected input");
    if (!args.password_stdin && !terminal.console_input())
        return error_for(SDP_INVALID_ARGUMENT, "Use --password-stdin for redirected input");
    Secret secret;
    if (terminal.console_input()) std::cerr << "Password: " << std::flush;
    const auto line = terminal.line(secret.bytes, 1024, true);
    if (terminal.console_input()) std::cerr << '\n';
    if (line == Line::cancelled) return error_for(SDP_CANCELLED, "Password entry cancelled");
    if (line != Line::ready || secret.bytes.empty())
        return error_for(SDP_INVALID_ARGUMENT, "Password input missing or invalid");
    return connection.open(args.profile, secret.bytes, terminal);
}

void error_message(const sdp_error& error)
{
    if (error.code != SDP_OK)
        std::cerr << terminal_text(error.message) << " (SDP " << error.code << ")\n";
}

void status_json(const Connection& connection, const Profile& profile, const sdp_error& error)
{
    Json result{{"connected", connection.connected() && error.code == SDP_OK},
                {"host", profile.host}, {"port", profile.port}, {"code", error.code}};
    if (connection.connected() && error.code == SDP_OK) result["capabilities"] = connection.features();
    std::cout << result.dump(-1, ' ', true, Json::error_handler_t::replace) << '\n';
}

void connected_message(const Profile& profile, const Terminal& terminal)
{
    terminal.success("Connected to " + terminal_text(profile.host) + " port " + std::to_string(profile.port));
}

Exit shell(Arguments& args, const Terminal& terminal, Connection& connection, sdp_error initial)
{
    auto last = exit_for(initial);
    if (initial.code != SDP_OK) error_message(initial);
    if (terminal.interactive() && !connection.connected()) std::cout << "Use \\connect PROFILE and \\help.\n";
    for (;;)
    {
        if (terminal.interrupted()) return Exit::cancelled;
        terminal.prompt(connection.connected());
        std::string command;
        switch (terminal.line(command, 4096))
        {
        case Line::eof: return last;
        case Line::cancelled: return Exit::cancelled;
        case Line::failed: return Exit::arguments;
        case Line::too_long: std::cerr << "Command exceeds 4096 bytes.\n"; last = Exit::arguments; continue;
        case Line::ready: break;
        }
        if (command.empty()) continue;
        if (command == "\\quit") return last;
        if (command == "\\help") { help(); last = Exit::ok; continue; }

        sdp_error result{};
        if (command == "\\connect" || command.starts_with("\\connect "sv))
        {
            try
            {
                if (command.starts_with("\\connect "sv))
                    args.profile = read_profile(std::filesystem::path(wide(std::string_view(command).substr(9))));
                result = connect(connection, args, terminal);
                if (result.code == SDP_OK) connected_message(args.profile, terminal);
            }
            catch (const std::invalid_argument& error) { result = error_for(SDP_INVALID_ARGUMENT, error.what()); }
        }
        else if (command == "\\capabilities" || command == "\\status")
        {
            result = connection.health(terminal);
            if (result.code == SDP_OK && command == "\\status") connected_message(args.profile, terminal);
            if (result.code == SDP_OK && command == "\\capabilities")
                for (const auto& [name, available] : connection.features().items())
                    std::cout << terminal_text(name) << ' ' << (available.get<bool>() ? "available" : "unavailable") << '\n';
        }
        else if (command.front() == '\\')
            result = error_for(SDP_INVALID_ARGUMENT, "Unknown command. Use \\help");
        else
            result = error_for(SDP_FEATURE_UNSUPPORTED, "SQL is unavailable in this client stage");

        error_message(result);
        last = exit_for(result);
        if (last == Exit::cancelled) return last;
    }
}
}

int wmain(int argc, wchar_t** argv)
{
    try
    {
        if (argc == 2 && std::wstring_view(argv[1]) == L"--help") { help(); return 0; }
        if (argc == 2 && std::wstring_view(argv[1]) == L"--version")
        {
            std::cout << "StarDataParadiseCLI 0.1.0\n";
            return 0;
        }
        auto args = arguments(argc, argv);
        Terminal terminal;
        if (argc == 1 && !terminal.interactive()) { help(); return 0; }
        terminal.set_color(args.color, args.json);
        Connection connection;
        auto result = args.connect ? connect(connection, args, terminal) : sdp_error{};
        if (args.status)
        {
            if (result.code == SDP_OK) result = connection.health(terminal);
            if (args.json) status_json(connection, args.profile, result);
            else if (result.code == SDP_OK) connected_message(args.profile, terminal);
            error_message(result);
            return static_cast<int>(exit_for(result));
        }
        if (result.code == SDP_OK && connection.connected()) connected_message(args.profile, terminal);
        if (result.code != SDP_OK && !terminal.interactive())
        {
            error_message(result);
            return static_cast<int>(exit_for(result));
        }
        return static_cast<int>(shell(args, terminal, connection, result));
    }
    catch (const std::invalid_argument& error)
    {
        std::cerr << "Invalid arguments. " << terminal_text(error.what()) << ". Use --help.\n";
        return static_cast<int>(Exit::arguments);
    }
    catch (...)
    {
        std::cerr << "CLI could not complete the operation.\n";
        return static_cast<int>(Exit::connection);
    }
}
