#include "automation.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>

#include <windows.h>

namespace
{
using namespace cli;
using namespace std::string_view_literals;
constexpr size_t script_limit = 16 * 1024 * 1024;

struct Arguments
{
    Profile profile;
    Color color = Color::automatic;
    Format format = Format::table;
    bool format_given{};
    bool status{};
    bool json{};
    bool password_stdin{};
    bool connect{};
    bool atomic{};
    bool stop{};
    bool prod{};
    std::optional<std::filesystem::path> file, output;
    std::optional<std::string> command;
    std::vector<std::string> bindings;
};

void help(std::ostream& out = std::cout)
{
    out << "Usage: StarDataParadiseCLI [--help | --version]\n"
           "       StarDataParadiseCLI --update-status FILE\n"
           "       StarDataParadiseCLI [--profile FILE] [--host HOST] [--port N] [--user USER]\n"
           "          [--ca PEM] [--tls-name NAME] [--timeout-ms N] [--password-stdin] [--prod]\n"
           "          [--status | --json] [--color auto|always|never]\n"
           "          [--file FILE | --command SQL] [--bind NAME=JSON] [--stop-on-error]\n"
           "          [--transaction each|single] [--format table|vertical|json|ndjson|csv] [--output FILE]\n"
           "Commands: \\connect [PROFILE], \\status, \\capabilities, \\databases, \\tables, \\describe NAME,\n"
           "          \\format NAME, \\timing on|off, \\run FILE, \\history [PREFIX], \\rpc GROUP ACTION [JSON],\n"
           "          \\autocommit on|off, \\begin, \\savepoint NAME, \\commit, \\rollback [NAME],\n"
           "          \\release NAME, \\outcome TOKEN, \\sessions, \\password [USER],\n"
           "          \\service ACTION NAME, \\help, \\quit\n"
           "RPC groups: sessions, history, decision, job, dataset, model, backup, admin\n";
}

bool journal_token(std::string_view text, std::string_view punctuation)
{
    const auto alphanumeric = [](char value) {
        return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
            (value >= '0' && value <= '9');
    };
    return !text.empty() && text.size() <= 64 && alphanumeric(text.front()) &&
        std::ranges::all_of(text, [&](char value) { return alphanumeric(value) || punctuation.find(value) != punctuation.npos; });
}

Json read_update_status(const std::filesystem::path& file)
{
    std::error_code error;
    const auto path = std::filesystem::absolute(file, error);
    const auto& name = path.native();
    if (error || name.size() < 4 || name[1] != L':' ||
        (name[2] != L'\\' && name[2] != L'/') || name.find(L':', 2) != name.npos)
        throw std::invalid_argument("Choose a regular file on a local drive");
    const wchar_t root[] = { name[0], L':', L'\\', L'\0' };
    const auto drive = GetDriveTypeW(root);
    if (drive != DRIVE_FIXED && drive != DRIVE_REMOVABLE && drive != DRIVE_RAMDISK && drive != DRIVE_CDROM)
        throw std::invalid_argument("Choose a regular file on a local drive");
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)) != 0)
        throw std::invalid_argument("Choose a regular file on a local drive");
    std::ifstream input(path, std::ios::binary);
    std::array<char, 64 * 1024 + 1> bytes{};
    if (!input || !input.read(bytes.data(), static_cast<std::streamsize>(bytes.size())).eof() || input.bad())
        throw std::invalid_argument("The update status file could not be read within 64 KiB");
    const auto source = Json::parse(bytes.begin(), bytes.begin() + input.gcount(), nullptr, false);
    const auto valid_phase = [](const std::string& phase) {
        constexpr std::array<std::string_view, 11> names{
            "planned", "verified", "preflight", "stopped", "staging", "staged", "verified_target",
            "started", "complete", "safe_stop", "rolled_back"};
        return std::ranges::find(names, std::string_view(phase)) != names.end();
    };
    if (!source.is_object() || !source.contains("schema") || source["schema"] != 1)
        throw std::invalid_argument("Invalid or unsupported update status");

    const auto field = [&](const char* key) -> std::string {
        return source.contains(key) && source[key].is_string() ? source[key].get<std::string>() : "";
    };
    const auto component = field("component");
    const auto from = field("from_version");
    const auto to = field("to_version");
    const auto phase = field("phase");
    const auto pending = field("pending");
    const auto boundary = field("rollback_boundary");
    const auto instance = field("instance");
    if ((component != "server" && component != "workbench" && component != "cli") ||
        !journal_token(from, ".+_-") || !journal_token(to, ".+_-") || !valid_phase(phase) ||
        (source.contains("pending") && !source["pending"].is_null() && !valid_phase(pending)) ||
        (boundary != "source_preserved" && boundary != "has_new_writes" &&
         boundary != "rollback_complete" && boundary != "unknown") ||
        !source.contains("requires_admin") || !source["requires_admin"].is_boolean() ||
        (source.contains("instance") && !source["instance"].is_null() && !journal_token(instance, "_.-")))
        throw std::invalid_argument("Invalid or unsupported update status");
    Json result{
        {"schema", 1}, {"component", component}, {"from_version", from}, {"to_version", to}, {"phase", phase},
        {"pending", pending.empty() ? Json(nullptr) : Json(pending)},
        {"rollback_boundary", boundary}, {"requires_admin", source["requires_admin"]}};
    if (!instance.empty()) result["instance"] = instance;
    return result;
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
        else if (option == L"--json") args.json = args.connect = true;
        else if (option == L"--password-stdin") args.password_stdin = true;
        else if (option == L"--stop-on-error") args.stop = true;
        else if (option == L"--prod") args.prod = true;
        else
        {
            if (i + 1 >= count) throw std::invalid_argument("Missing option value");
            const std::wstring_view value(values[++i]);
            if (option == L"--color") args.color = color_mode(utf8(value));
            else if (option == L"--profile") path = value;
            else if (option == L"--host") host = utf8(value);
            else if (option == L"--port") port = number(utf8(value), 1, 65535);
            else if (option == L"--user") user = utf8(value);
            else if (option == L"--ca") ca = utf8(value);
            else if (option == L"--tls-name") tls_name = utf8(value);
            else if (option == L"--timeout-ms") timeout = number(utf8(value), 100, 120000);
            else if (option == L"--file") args.file = value;
            else if (option == L"--command") args.command = utf8(value);
            else if (option == L"--output") args.output = value;
            else if (option == L"--format") { args.format = parse_format(utf8(value)); args.format_given = true; }
            else if (option == L"--bind") args.bindings.push_back(utf8(value));
            else if (option == L"--transaction")
            {
                if (value != L"single" && value != L"each") throw std::invalid_argument("Use --transaction each or single");
                args.atomic = value == L"single";
            }
            else throw std::invalid_argument("Unknown option");
        }
    }
    if (args.file && args.command) throw std::invalid_argument("Choose one input source");
    if (args.status && (args.file || args.command)) throw std::invalid_argument("--status does not consume SQL");
    if (args.json && !(args.file || args.command || args.format_given)) args.status = true;
    if (args.json && !args.status) args.format = Format::json;
    const bool local_only = args.command && args.command->starts_with("\\service ") &&
        args.command->find_first_of("\r\n") == std::string::npos && !args.atomic;
    args.connect |= args.file.has_value() || (!local_only && (args.command.has_value() || args.format_given)) || args.atomic ||
        path.has_value() || host.has_value() || user.has_value() || ca.has_value() || tls_name.has_value() ||
        timeout.has_value() || port.has_value() || args.password_stdin || args.status;
    if (path) args.profile = read_profile(*path);
    if (host) args.profile.host = std::move(*host);
    if (user) args.profile.user = std::move(*user);
    if (ca) args.profile.ca = std::move(*ca);
    if (tls_name) args.profile.tls_name = std::move(*tls_name);
    if (port) args.profile.port = static_cast<uint16_t>(*port);
    if (timeout) args.profile.timeout = *timeout;
    if (args.prod) args.profile.production = true;
    return args;
}

sdp_error connect(Connection& connection, const Arguments& args, const Terminal& terminal)
{
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

void status_json(const Connection& connection, const Profile& profile, const sdp_error& error, std::ostream& stream)
{
    Json result{{"connected", connection.connected() && error.code == SDP_OK},
                {"host", profile.host}, {"port", profile.port}, {"code", error.code}};
    if (connection.connected() && error.code == SDP_OK)
    {
        result["capabilities"] = connection.features();
        result["session"] = connection.session();
    }
    stream << result.dump() << '\n';
}

std::string connected_message(const Profile& profile)
{
    return "Connected to " + terminal_text(profile.host) + " port " + std::to_string(profile.port);
}

std::string_view strip(std::string_view value)
{
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == value.npos) return {};
    return value.substr(begin, value.find_last_not_of(" \t\r\n") - begin + 1);
}

std::pair<std::string_view, std::string_view> first(std::string_view line)
{
    line = strip(line);
    const auto end = line.find_first_of(" \t");
    if (end == line.npos) return {line, {}};
    return {line.substr(0, end), strip(line.substr(end))};
}

bool safe_history(std::string_view input)
{
    input = strip(input);
    if (input.size() > 1024 || input.find_first_of("'\"\n\r") != input.npos ||
        input.find("--") != input.npos || input.find("/*") != input.npos) return false;
    const auto [command, rest] = first(input);
    if (command == "\\status" || command == "\\tables" || command == "\\databases" ||
        command == "\\capabilities" || command == "\\help") return rest.empty();
    std::string upper(input);
    for (auto& ch : upper) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return upper.starts_with("SELECT ") && upper.find("PASSWORD") == upper.npos &&
        upper.find("SECRET") == upper.npos && upper.find("TOKEN") == upper.npos;
}

struct ServiceHandle
{
    SC_HANDLE value;
    ~ServiceHandle() { if (value) CloseServiceHandle(value); }
};

Reply service_error(const char* message)
{
    const auto status = GetLastError();
    if (status == ERROR_ACCESS_DENIED) return {error_for(SDP_PERMISSION_DENIED, message)};
    if (status == ERROR_SERVICE_DOES_NOT_EXIST) return {error_for(SDP_INVALID_ARGUMENT, "StarDataParadise service not installed")};
    return {error_for(SDP_INVALID_STATE, message)};
}

Reply local_service(std::string_view arguments, const Terminal& terminal, Output& output, uint32_t timeout)
{
    const auto [verb, name] = first(arguments);
    if ((verb != "status" && verb != "start" && verb != "stop") || name.size() > 128 ||
        !name.starts_with("StarDataParadise") ||
        name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != name.npos)
        return {error_for(SDP_INVALID_ARGUMENT, "Use \\service status|start|stop StarDataParadiseServiceName")};

    const ServiceHandle manager{OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)};
    if (!manager.value) return service_error("Cannot open the local Service Control Manager");
    const auto access = SERVICE_QUERY_STATUS | (verb == "start" ? SERVICE_START : verb == "stop" ? SERVICE_STOP : 0);
    const auto service_name = wide(name);
    const ServiceHandle service{OpenServiceW(manager.value, service_name.c_str(), access)};
    if (!service.value) return service_error("Cannot open the local StarDataParadise service");

    SERVICE_STATUS_PROCESS state{};
    DWORD bytes{};
    const auto query = [&] {
        return QueryServiceStatusEx(service.value, SC_STATUS_PROCESS_INFO,
            reinterpret_cast<BYTE*>(&state), sizeof(state), &bytes) != 0;
    };
    if (!query()) return service_error("Cannot query local service status");

    if (verb != "status")
    {
        const DWORD desired = verb == "start" ? SERVICE_RUNNING : SERVICE_STOPPED;
        if (state.dwCurrentState != desired)
        {
            if (verb == "start" && state.dwCurrentState != SERVICE_START_PENDING &&
                !StartServiceW(service.value, 0, nullptr))
                return service_error("Local service start failed");
            if (verb == "stop" && state.dwCurrentState != SERVICE_STOP_PENDING)
            {
                SERVICE_STATUS transition{};
                if (!ControlService(service.value, SERVICE_CONTROL_STOP, &transition))
                    return service_error("Local service stop failed");
            }
            const auto started = GetTickCount64();
            do
            {
                if (WaitForSingleObject(terminal.interrupt(), 100) == WAIT_OBJECT_0)
                    return {error_for(SDP_CANCELLED, "Local service status wait cancelled")};
                if (!query()) return service_error("Cannot query local service status");
                if (state.dwCurrentState == desired) break;
                if (GetTickCount64() - started >= timeout)
                    return {error_for(SDP_TIMEOUT, "Local service did not reach the requested state")};
            } while (state.dwCurrentState == SERVICE_START_PENDING || state.dwCurrentState == SERVICE_STOP_PENDING);
            if (state.dwCurrentState != desired) return {error_for(SDP_INVALID_STATE, "Local service transition failed")};
        }
    }
    constexpr const char* states[]{"unknown", "stopped", "start_pending", "stop_pending", "running",
        "continue_pending", "pause_pending", "paused"};
    const auto service_state = state.dwCurrentState < std::size(states) ? states[state.dwCurrentState] : states[0];
    output.emit("service", Json{{"name", name}, {"state", service_state}, {"pid", state.dwProcessId}});
    return {};
}

class Runner
{
public:
    Runner(Arguments& args, const Terminal& terminal, Connection& connection, Automation& engine, Output& output)
        : args_(args), terminal_(terminal), connection_(connection), engine_(engine), output_(output) {}

    void file(const std::filesystem::path& path)
    {
        if (depth_ >= 8) { failed(SDP_INVALID_ARGUMENT, "Too many nested script files"); return; }
        std::ifstream input(path, std::ios::binary);
        if (!input) { failed(SDP_INVALID_ARGUMENT, "Cannot open script file"); return; }
        ++depth_;
        source(input);
        --depth_;
    }

    void source(std::istream& input)
    {
        std::string line, pending;
        bool first_line = true;
        while (!stopped() && std::getline(input, line))
        {
            if (first_line && line.starts_with("\xef\xbb\xbf")) line.erase(0, 3);
            first_line = false;
            process(line, pending);
        }
        if (input.bad()) failed(SDP_INVALID_ARGUMENT, "Could not read script");
        if (!stopped()) sql(pending, true);
    }

    void shell()
    {
        if (terminal_.interactive() && !connection_.connected())
            std::cerr << "Use \\connect PROFILE and \\help.\n";
        std::string pending;
        while (!stopped())
        {
            prompt(pending.empty());
            std::string line;
            switch (terminal_.line(line, 64 * 1024, false, &history_))
            {
            case Line::eof: sql(pending, true); return;
            case Line::cancelled: failed(SDP_CANCELLED, "Operation cancelled"); return;
            case Line::failed: failed(SDP_INVALID_ARGUMENT, "Cannot read input"); return;
            case Line::too_long: failed(SDP_INVALID_ARGUMENT, "Input line exceeds 65536 bytes"); break;
            case Line::ready: process(line, pending); break;
            }
        }
    }

    bool stopped() const
    {
        if (quit_ || status_ == Exit::cancelled) return true;
        if (interactive_shell()) return false;
        return status_ != Exit::ok &&
            (args_.stop || args_.atomic || status_ == Exit::unknown || !connection_.connected());
    }

    Exit status() const { return status_; }

    void result(const Reply& reply, std::string_view origin = {})
    {
        if (reply.error.code != SDP_OK)
        {
            const auto next = reply.status();
            if (status_ == Exit::ok || next == Exit::unknown || next == Exit::cancelled) status_ = next;
            engine_.report(reply, origin);
        }
        output_.flush();
    }

private:
    bool interactive_shell() const
    {
        return terminal_.interactive() && !args_.file && !args_.command && !args_.atomic;
    }

    void remember(std::string_view input)
    {
        input = strip(input);
        if (!interactive_shell() || !safe_history(input)) return;
        if (!history_.empty() && history_.back() == input) return;
        if (history_.size() == 64) history_.erase(history_.begin());
        history_.emplace_back(input);
    }

    void failed(sdp_status code, const char* message, bool sql = false)
    {
        result({error_for(code, message), {}, sql});
    }

    void process(std::string_view line, std::string& pending)
    {
        if (pending.empty() && strip(line).starts_with('\\'))
        {
            command(strip(line).substr(1));
            remember(line);
            return;
        }
        if (pending.size() + line.size() + 1 > script_limit)
        {
            failed(SDP_INVALID_ARGUMENT, "SQL statement exceeds the script size limit");
            pending.clear();
            return;
        }
        pending.append(line);
        pending += '\n';
        sql(pending, false);
    }

    void sql(std::string& pending, bool end)
    {
        const auto statements = split_sql(pending, end);
        for (const auto statement : statements.statements)
        {
            if (stopped()) break;
            const auto reply = engine_.execute(statement);
            result(reply, statement);
            if (reply.error.code == SDP_OK) remember(statement);
        }
        if (end && statements.incomplete && !stopped())
            failed(SDP_INVALID_ARGUMENT, "Incomplete SQL literal, comment, or PL/SQL block", true);
        pending.erase(0, statements.consumed);
    }

    void command(std::string_view line)
    {
        const auto [name, argument] = first(line);
        try
        {
            if (name == "quit") { quit_ = true; return; }
            if (name == "help") { help(std::cerr); return; }
            if (name == "run")
            {
                if (argument.empty()) { failed(SDP_INVALID_ARGUMENT, "Use \\run FILE"); return; }
                auto path = argument;
                if (path.size() >= 2 && path.front() == '"' && path.back() == '"') path = path.substr(1, path.size() - 2);
                file(std::filesystem::path(wide(path)));
                return;
            }
            if (name == "history")
            {
                auto matches = Json::array();
                for (const auto& item : history_) if (item.starts_with(argument)) matches.push_back(item);
                output_.emit("terminal_history", matches);
                output_.flush();
                return;
            }
            if (name == "format")
            {
                const auto selected = parse_format(argument);
                if (args_.format_given && !interactive_shell() && selected != args_.format)
                    throw std::invalid_argument("The batch output format was already selected");
                if (selected != args_.format) { output_.format(selected); args_.format = selected; }
                return;
            }
            if (name == "timing")
            {
                if (argument != "on" && argument != "off") { failed(SDP_INVALID_ARGUMENT, "Use \\timing on or off"); return; }
                engine_.timing(argument == "on");
                return;
            }
            if (name == "connect") { reconnect(argument); return; }
            if (name == "password") { password(argument); return; }
            if (name == "service")
            {
                result(local_service(argument, terminal_, output_, args_.profile.timeout));
                return;
            }
            if (name == "rpc")
            {
                const auto [group, rest] = first(argument);
                const auto [action, body] = first(rest);
                result(engine_.management(group, action, body));
                return;
            }
            if (args_.atomic && (name == "begin" || name == "commit" || name == "rollback" ||
                name == "savepoint" || name == "release" || name == "autocommit"))
            {
                failed(SDP_INVALID_STATE, "Transaction commands are unavailable with --transaction single");
                return;
            }
            if (name == "rollback" && !argument.empty()) result(engine_.control("rollback-to", argument));
            else result(engine_.control(name, argument));
        }
        catch (const std::invalid_argument& error) { failed(SDP_INVALID_ARGUMENT, error.what()); }
    }

    void reconnect(std::string_view argument)
    {
        if (engine_.active() || args_.atomic)
        {
            failed(SDP_INVALID_STATE, "Commit or roll back the active transaction before reconnecting");
            return;
        }
        const auto previous = args_.profile;
        if (!argument.empty()) args_.profile = read_profile(std::filesystem::path(wide(argument)));
        const auto error = connect(connection_, args_, terminal_);
        if (error.code == SDP_OK)
        {
            engine_.timeout(args_.profile.timeout);
            if (interactive_shell() && (status_ == Exit::auth || status_ == Exit::connection)) status_ = Exit::ok;
            if (terminal_.interactive() && !args_.format_given && !args_.output)
                terminal_.success(connected_message(args_.profile));
        }
        else if (connection_.connected()) args_.profile = previous;
        result({error});
    }

    void password(std::string_view user)
    {
        if (!interactive_shell() || !connection_.connected() || engine_.active())
        {
            failed(SDP_INVALID_STATE, "Password change needs a connected console and no active transaction");
            return;
        }
        if (user.empty()) user = args_.profile.user;
        const auto read_secret = [&](std::string_view prompt, Secret& secret) {
            std::cerr << prompt << std::flush;
            const auto line = terminal_.line(secret.bytes, 1024, true);
            std::cerr << '\n';
            if (line == Line::ready && !secret.bytes.empty()) return true;
            failed(line == Line::cancelled ? SDP_CANCELLED : SDP_INVALID_ARGUMENT, "Password entry did not complete");
            return false;
        };
        Secret previous, fresh, confirmation;
        if (user == args_.profile.user && !read_secret("Current password: ", previous)) return;
        if (!read_secret("New password: ", fresh) || !read_secret("Confirm password: ", confirmation)) return;
        if (fresh.bytes != confirmation.bytes)
        {
            failed(SDP_INVALID_ARGUMENT, "Passwords did not match");
            return;
        }
        result(engine_.password(user, previous.bytes, fresh.bytes));
    }

    void prompt(bool fresh) const
    {
        if (!connection_.connected()) { terminal_.prompt(false, fresh ? "" : "continue"); return; }
        const auto id = connection_.session().value("session_id", "");
        const std::string detail = fresh ?
            "main.public " + id.substr(0, 8) + (engine_.active() ? " tx" : engine_.autocommit() ? " auto" : " manual") +
            (args_.profile.production ? " PROD" : " DEV") : "continue";
        terminal_.prompt(true, detail);
    }

    Arguments& args_;
    const Terminal& terminal_;
    Connection& connection_;
    Automation& engine_;
    Output& output_;
    std::vector<std::string> history_;
    Exit status_ = Exit::ok;
    int depth_{};
    bool quit_{};
};

int run(Arguments& args, Terminal& terminal)
{
    const bool machine = args.json || args.format == Format::json || args.format == Format::ndjson ||
        args.format == Format::csv || args.output.has_value();
    terminal.set_color(args.color, machine);
    terminal.quiet_input(machine);
    if (args.atomic && terminal.interactive() && !args.file && !args.command)
        throw std::invalid_argument("--transaction single requires a batch input");
    std::ofstream file;
    if (args.output)
    {
        std::error_code ignore;
        if (args.file && (std::filesystem::equivalent(*args.file, *args.output, ignore) ||
            std::filesystem::absolute(*args.file).lexically_normal() == std::filesystem::absolute(*args.output).lexically_normal()))
            throw std::invalid_argument("Output must be different from the script file");
        file.open(*args.output, std::ios::binary | std::ios::trunc);
        if (!file) throw std::invalid_argument("Cannot open output file");
    }
    std::ostream& stream = args.output ? file : std::cout;
    Connection connection;
    if (args.status)
    {
        auto error = connect(connection, args, terminal);
        if (error.code == SDP_OK) error = connection.health(terminal);
        if (args.json || args.format == Format::json) status_json(connection, args.profile, error, stream);
        else if (error.code == SDP_OK)
        {
            if (args.output) stream << connected_message(args.profile) << '\n';
            else terminal.success(connected_message(args.profile));
        }
        error_message(error);
        return static_cast<int>(exit_for(error));
    }

    Output output(stream, args.format);
    Automation automation(connection, terminal, output, args.profile.timeout);
    Runner runner(args, terminal, connection, automation, output);
    try
    {
        for (const auto& binding : args.bindings) automation.bind(binding);
        const auto error = args.connect ? connect(connection, args, terminal) : sdp_error{};
        if (error.code != SDP_OK) runner.result({error});
        if (terminal.interactive() && !args.file && !args.command && !args.format_given &&
            !args.output && connection.connected())
            terminal.success(connected_message(args.profile));
        if (args.atomic && runner.status() == Exit::ok)
        {
            runner.result(automation.control("begin"));
            automation.set_atomic_script(true);
        }
        if (!runner.stopped())
        {
            if (args.file) runner.file(*args.file);
            else if (args.command)
            {
                std::istringstream script(*args.command);
                runner.source(script);
            }
            else runner.shell();
        }
        if (automation.active())
        {
            if (args.atomic && runner.status() == Exit::ok)
                runner.result(automation.control("commit"));
            else
            {
                runner.result(automation.control("rollback"));
                if (!args.atomic && runner.status() == Exit::ok)
                    runner.result({error_for(SDP_INVALID_STATE, "Active transaction rolled back at end of input")});
            }
        }
    }
    catch (const std::invalid_argument& error)
    {
        runner.result({error_for(SDP_INVALID_ARGUMENT, error.what())});
    }
    catch (const Json::exception&)
    {
        runner.result({error_for(SDP_MALFORMED_MESSAGE, "Unexpected SDK response shape")});
    }
    output.finish(runner.status());
    return static_cast<int>(runner.status());
}
}

int wmain(int argc, wchar_t** argv)
{
    try
    {
        if (argc == 2 && std::wstring_view(argv[1]) == L"--help") { help(); return 0; }
        if (argc == 2 && std::wstring_view(argv[1]) == L"--version")
        {
            std::cout << "StarDataParadiseCLI 0.2.0\n";
            return 0;
        }
        if (argc >= 2 && std::wstring_view(argv[1]) == L"--update-status")
        {
            if (argc != 3) throw std::invalid_argument("Use --update-status FILE");
            std::cout << read_update_status(argv[2]).dump() << '\n';
            return 0;
        }
        auto args = arguments(argc, argv);
        Terminal terminal;
        if (argc == 1 && !terminal.interactive()) { help(); return 0; }
        return run(args, terminal);
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
