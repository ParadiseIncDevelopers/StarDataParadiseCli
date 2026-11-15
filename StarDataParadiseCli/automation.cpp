#include "automation.hpp"

#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <iostream>
#include <set>

namespace cli
{
namespace
{
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

std::string request_token()
{
    std::array<UCHAR, 16> bytes{};
    if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
            BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        throw std::runtime_error("Cannot generate a request token");
    constexpr std::string_view hex = "0123456789abcdef";
    std::string token;
    for (auto byte : bytes)
    {
        token += hex[byte >> 4];
        token += hex[byte & 15];
    }
    return token;
}

bool name_start(unsigned char c) { return std::isalpha(c) || c == '_'; }
bool name_part(unsigned char c) { return name_start(c) || std::isdigit(c); }

struct SqlCode
{
    std::vector<std::string> leading;
    std::vector<std::string_view> original;
    std::set<std::string> parameters;
    size_t words{};
    bool simple = true;
};

SqlCode inspect(std::string_view sql)
{
    SqlCode code;
    for (size_t i = 0; i < sql.size();)
    {
        const char c = sql[i++];
        if (c == '\'' || c == '"')
        {
            code.simple = false;
            while (i < sql.size())
            {
                if (sql[i++] != c) continue;
                if (i >= sql.size() || sql[i] != c) break;
                ++i;
            }
        }
        else if (c == '-' && i < sql.size() && sql[i] == '-')
        {
            const auto end = sql.find('\n', i + 1);
            i = end == sql.npos ? sql.size() : end + 1;
        }
        else if (c == '/' && i < sql.size() && sql[i] == '*')
        {
            const auto end = sql.find("*/", i + 1);
            i = end == sql.npos ? sql.size() : end + 2;
        }
        else if (c == ':' && i < sql.size() && name_start(sql[i]))
        {
            code.simple = false;
            const auto start = i++;
            while (i < sql.size() && name_part(sql[i])) ++i;
            code.parameters.emplace(sql.substr(start, i - start));
        }
        else if (name_start(c))
        {
            const auto start = i - 1;
            while (i < sql.size() && name_part(sql[i])) ++i;
            ++code.words;
            if (code.leading.size() < 4)
            {
                const auto original = sql.substr(start, i - start);
                auto word = std::string(original);
                for (auto& ch : word) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                code.leading.push_back(std::move(word));
                code.original.push_back(original);
            }
        }
        else if (c != ';' && !std::isspace(static_cast<unsigned char>(c))) code.simple = false;
    }
    return code;
}

std::string_view strip(std::string_view input)
{
    const auto begin = input.find_first_not_of(" \t\r\n");
    if (begin == input.npos) return {};
    return input.substr(begin, input.find_last_not_of(" \t\r\n") - begin + 1);
}

Reply failure(sdp_status code, const char* reason, bool sql = false)
{
    return {error_for(code, reason), {}, sql};
}

Json declaration(const Json& typed)
{
    const auto kind = typed.at("t").get<std::string>();
    if (kind == "i64") return {{"type", "BIGINT"}};
    if (kind == "text") return {{"type", "TEXT"}};
    if (kind == "bool") return {{"type", "BOOLEAN"}};
    if (kind == "decimal") return {{"type", "DECIMAL"}, {"precision", 38}, {"scale", typed.at("scale")}};
    if (kind == "timestamp") return {{"type", "TIMESTAMP WITH TIME ZONE"}};
    if (kind == "binary") return {{"type", "BLOB"}};
    if (kind == "date") return {{"type", "DATE"}};
    if (kind == "time") return {{"type", "TIME"}};
    if (kind == "record") return {{"type", "RECORD"}};
    if (kind == "collection") return {{"type", "COLLECTION"}};
    throw std::invalid_argument("A NULL bind requires explicit type and value objects");
}
}

Automation::Automation(Connection& connection, const Terminal& terminal, Output& output, uint32_t timeout)
    : connection_(connection), terminal_(terminal), output_(output), timeout_(timeout) {}

void Automation::bind(std::string_view specification)
{
    const auto equal = specification.find('=');
    const auto name = specification.substr(0, equal);
    if (equal == specification.npos || name.empty() || name.size() > 64 ||
        !name_start(name.front()) || !std::ranges::all_of(name, name_part) || bindings_.contains(name))
        throw std::invalid_argument("Expected a unique --bind NAME=JSON");
    try
    {
        auto typed = Json::parse(specification.substr(equal + 1));
        if (!typed.is_object()) throw std::invalid_argument("A wire value object is required");
        if (!typed.contains("type"))
        {
            auto type = declaration(typed);
            typed = Json{{"type", std::move(type)}, {"value", std::move(typed)}};
        }
        if (!typed.at("type").is_object() || !typed.at("type").at("type").is_string() ||
            !typed.at("value").is_object() || !typed.at("value").at("t").is_string())
            throw std::invalid_argument("A bind requires a declaration and wire value");
        bindings_[std::string(name)] = std::move(typed);
    }
    catch (const Json::exception&) { throw std::invalid_argument("Invalid --bind JSON structure"); }
}

Reply Automation::invoke(const Connection::Request& operation, bool sql)
{
    auto error = connection_.request(terminal_, operation);
    if (!connection_.connected()) active_ = false;
    return {error, {}, sql};
}

Reply Automation::receipt(std::string_view kind,
    const std::function<sdp_status(sdp_connection, sdp_buffer*, sdp_error*)>& operation,
    std::string_view token, bool sql)
{
    Buffer output;
    auto reply = invoke([&](auto handle, auto error) { return operation(handle, &output.value, error); }, sql);
    if (reply.error.unknown_outcome) reply.token = token;
    if (reply.error.code == SDP_OK) emit(kind, output.json());
    return reply;
}

void Automation::emit(std::string_view kind, const Json& data)
{
    output_.emit(kind, data);
}

Reply Automation::stream(std::string_view result_id)
{
    for (;;)
    {
        Buffer page;
        auto reply = invoke([&](auto handle, auto error) {
            return sdp_fetch(handle, text(result_id), 64, &page.value, error);
        }, true);
        if (reply.error.code != SDP_OK)
        {
            if (connection_.connected()) invoke([&](auto handle, auto error) {
                return sdp_result_cancel(handle, text(result_id), error);
            }, true);
            return reply;
        }
        const auto batch = page.json().at("batch");
        emit("batch", batch);
        if (batch.at("end_of_results").get<bool>()) return reply;
    }
}

Reply Automation::sql(std::string_view source, bool token_needed, bool embedded_write)
{
    const auto code = inspect(source);
    auto declarations = Json::object(), values = Json::object();
    for (const auto& name : code.parameters)
    {
        const auto entry = bindings_.find(name);
        if (entry == bindings_.end()) continue;
        declarations[name] = entry->at("type");
        values[name] = entry->at("value");
    }
    const auto types_json = declarations.dump(), values_json = values.dump();

    Buffer statement;
    auto reply = invoke([&](auto handle, auto error) {
        return sdp_prepare_typed(handle, text(source), text(types_json), &statement.value, error);
    }, true);
    if (reply.error.code != SDP_OK) return reply;
    const auto token = token_needed ? request_token() : std::string{};
    Buffer execution;
    reply = invoke([&](auto handle, auto error) {
        return sdp_execute(handle, text(statement.bytes()), text(values_json), text(token),
            1'000'000, &execution.value, error);
    }, true);
    if (reply.error.unknown_outcome) reply.token = token;
    if (embedded_write && (reply.error.code == SDP_CANCELLED || reply.error.code == SDP_TIMEOUT ||
        reply.error.code == SDP_CONNECTION_LOST || reply.error.code == SDP_MALFORMED_MESSAGE))
        reply.error.unknown_outcome = 1;
    if (reply.error.code == SDP_OK)
    {
        const auto received = execution.json();
        emit("execute", received);
        if (received.contains("result_id")) reply = stream(received.at("result_id").get<std::string>());
    }
    if (connection_.connected())
    {
        const auto closed = invoke([&](auto handle, auto error) {
            return sdp_statement_close(handle, text(statement.bytes()), error);
        }, true);
        if (reply.error.code == SDP_OK) reply = closed;
    }
    return reply;
}

Reply Automation::execute(std::string_view source)
{
    const auto started = Clock::now();
    const auto code = inspect(source);
    const auto& words = code.leading;
    if (words.empty()) return failure(SDP_INVALID_ARGUMENT, "Empty SQL statement", true);
    const auto& verb = words[0];
    const std::string_view next = words.size() > 1 ? words[1] : "";
    std::string_view control_name, argument;
    const bool standalone_begin = verb == "BEGIN" && (next.empty() || next == "TRANSACTION" || next == "WORK");
    if (standalone_begin || verb == "COMMIT" || verb == "ROLLBACK" || verb == "SAVEPOINT" || verb == "RELEASE")
    {
        if (code.simple)
        {
            if (standalone_begin && code.words <= 2) control_name = "begin";
            else if ((verb == "COMMIT" || verb == "ROLLBACK") &&
                (code.words == 1 || (code.words == 2 && next == "WORK")))
                control_name = verb == "COMMIT" ? "commit" : "rollback";
            else if (verb == "SAVEPOINT" && code.words == 2) control_name = "savepoint";
            else if (verb == "RELEASE" && code.words == 3 && next == "SAVEPOINT") control_name = "release";
            else if (verb == "ROLLBACK" && next == "TO" &&
                ((code.words == 3 && words[2] != "SAVEPOINT") ||
                (code.words == 4 && words[2] == "SAVEPOINT"))) control_name = "rollback-to";
        }
        if (control_name.empty()) return failure(SDP_INVALID_ARGUMENT, "Invalid transaction statement", true);
        if (control_name == "savepoint" || control_name == "release" || control_name == "rollback-to")
            argument = code.original.back();
    }

    Reply reply;
    if (!control_name.empty())
    {
        if (atomic_script_) return failure(SDP_INVALID_STATE, "Transaction commands are unavailable with --transaction single", true);
        reply = control(control_name, argument);
    }
    else
    {
        const bool routine = verb == "CALL" || verb == "DECLARE" || verb == "BEGIN";
        const bool special = verb == "HISTORY" || verb == "DECISION" || verb == "TRAIN" || verb == "EVALUATE" ||
            verb == "PREDICT" || ((verb == "CREATE" || verb == "ALTER") && next == "MODEL");
        const bool embedded_write = verb == "TRAIN" || ((verb == "CREATE" || verb == "ALTER") && next == "MODEL") ||
            (verb == "HISTORY" && (next == "RETAIN" || next == "PURGE" || next == "PIN" || next == "UNPIN" ||
                ((next == "RESTORE" || next == "REVERT") && words.size() > 2 && words[2] == "APPLY")));
        const bool read = verb == "SELECT" || verb == "WITH" || verb == "EXPLAIN";
        const bool owned = routine && autocommit_ && !active();
        if ((!autocommit_ || routine) && !active())
        {
            reply = control("begin");
            if (reply.error.code != SDP_OK) return reply;
        }
        reply = sql(source, !active() && !read && !routine && !special, embedded_write);
        if (owned && active())
        {
            const auto end = control(reply.error.code == SDP_OK ? "commit" : "rollback");
            if (reply.error.code == SDP_OK) reply = end;
        }
    }
    if (timing_)
    {
        const auto elapsed = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
        std::cerr << "Time: " << elapsed << " ms\n";
    }
    return reply;
}

Reply Automation::control(std::string_view command, std::string_view argument)
{
    argument = strip(argument);
    if (command == "autocommit")
    {
        if (argument != "on" && argument != "off")
            return failure(SDP_INVALID_ARGUMENT, "Use autocommit on or off", true);
        if (argument == "on" && active())
            return failure(SDP_INVALID_STATE, "Commit or roll back the active transaction first");
        autocommit_ = argument == "on";
        emit("transaction", Json{{"autocommit", autocommit_}});
        return {};
    }
    if (command == "begin")
    {
        if (active()) return failure(SDP_INVALID_STATE, "A transaction is already active");
        auto reply = invoke(sdp_tx_begin, true);
        if (reply.error.code == SDP_OK)
        {
            active_ = true;
            emit("transaction", Json{{"status", "active"}});
        }
        return reply;
    }
    if (command == "commit")
    {
        if (!active()) return failure(SDP_INVALID_STATE, "No active transaction");
        const auto token = request_token();
        auto reply = receipt("transaction", [&](auto handle, auto buffer, auto error) {
            return sdp_tx_commit(handle, text(token), buffer, error);
        }, token);
        active_ = false;
        return reply;
    }
    if (command == "rollback")
    {
        if (!active()) return failure(SDP_INVALID_STATE, "No active transaction");
        auto reply = invoke(sdp_tx_rollback, true);
        active_ = false;
        if (reply.error.code == SDP_OK) emit("transaction", Json{{"status", "rolled_back"}});
        return reply;
    }
    if (command == "savepoint" || command == "release" || command == "rollback-to")
    {
        if (!active()) return failure(SDP_INVALID_STATE, "No active transaction");
        const auto action = command == "savepoint" ? sdp_tx_savepoint :
            command == "release" ? sdp_tx_release : sdp_tx_rollback_to;
        auto reply = invoke([&](auto handle, auto error) { return action(handle, text(argument), error); }, true);
        if (reply.error.code == SDP_OK) emit("transaction", Json{{"action", command}, {"name", argument}});
        return reply;
    }
    if (command == "outcome")
    {
        Buffer output;
        auto reply = invoke([&](auto handle, auto error) {
            return sdp_tx_outcome(handle, text(argument), &output.value, error);
        }, true);
        if (reply.error.code != SDP_OK) return reply;
        const auto outcome = output.json();
        emit("outcome", outcome);
        if (outcome.at("status") == "unknown")
        {
            reply = failure(SDP_INVALID_STATE, "Server has no durable outcome for this token");
            reply.error.unknown_outcome = 1;
        }
        return reply;
    }
    if (command == "sessions")
        return receipt("sessions", sdp_session_list);
    if (command == "status" || command == "capabilities")
    {
        auto reply = invoke(sdp_health);
        if (reply.error.code == SDP_OK)
        {
            if (command == "capabilities") emit("capabilities", connection_.features());
            else emit("status", Json{{"connected", true}, {"session", connection_.session()},
                {"database", "main"}, {"schema", "public"}, {"transaction_active", active_}, {"autocommit", autocommit_}});
        }
        return reply;
    }
    if (command == "databases" || command == "tables" || command == "describe")
    {
        Buffer received;
        auto reply = invoke([&](auto handle, auto error) { return sdp_metadata(handle, &received.value, error); }, true);
        if (reply.error.code != SDP_OK) return reply;
        auto items = Json::array();
        const auto metadata = received.json();
        for (const auto& object : metadata.at("objects"))
        {
            int kind = object.at("kind").get<int>();
            const auto& parts = object.at("qualified_name");
            if (command == "databases" && kind != 1) continue;
            if (command == "tables" && kind != 3 && kind != 5) continue;
            if (command == "describe")
            {
                std::string full;
                for (const auto& part : parts)
                {
                    if (!full.empty()) full += '.';
                    full += part.get<std::string>();
                }
                if (argument.empty() || (full != argument && parts.back().get<std::string>() != argument)) continue;
            }
            items.push_back(object);
        }
        if (command == "describe" && items.empty())
            return failure(SDP_INVALID_ARGUMENT, "No visible catalog object matches the name", true);
        emit(command, items);
        return reply;
    }
    return failure(SDP_INVALID_ARGUMENT, "Unknown CLI command");
}

namespace
{
using Rpc = sdp_status(SDP_CALL*)(sdp_connection, uint32_t, sdp_text, sdp_buffer*, sdp_error*);

struct RpcMethod
{
    std::string_view group, name;
    Rpc function;
    uint32_t method;
    bool token = false;
};

constexpr auto rpc_methods = std::to_array<RpcMethod>({
    {"history", "log", sdp_history_command, SDP_HISTORY_LOG},
    {"history", "diff", sdp_history_command, SDP_HISTORY_DIFF},
    {"history", "read", sdp_history_command, SDP_HISTORY_READ},
    {"history", "verify", sdp_history_command, SDP_HISTORY_VERIFY},
    {"history", "status", sdp_history_command, SDP_HISTORY_STATUS},
    {"history", "retain", sdp_history_command, SDP_HISTORY_RETAIN},
    {"history", "purge", sdp_history_command, SDP_HISTORY_PURGE},
    {"history", "pin", sdp_history_command, SDP_HISTORY_PIN},
    {"history", "unpin", sdp_history_command, SDP_HISTORY_UNPIN},
    {"history", "restore", sdp_history_command, SDP_HISTORY_RESTORE},
    {"history", "revert", sdp_history_command, SDP_HISTORY_REVERT},
    {"decision", "create", sdp_decision_command, SDP_DECISION_CREATE, true},
    {"decision", "validate", sdp_decision_command, SDP_DECISION_VALIDATE},
    {"decision", "activate", sdp_decision_command, SDP_DECISION_ACTIVATE, true},
    {"decision", "test", sdp_decision_command, SDP_DECISION_TEST},
    {"decision", "explain", sdp_decision_command, SDP_DECISION_EXPLAIN},
    {"decision", "whynot", sdp_decision_command, SDP_DECISION_WHYNOT},
    {"decision", "whatif", sdp_decision_command, SDP_DECISION_WHATIF},
    {"decision", "recheck", sdp_decision_command, SDP_DECISION_RECHECK},
    {"job", "submit", sdp_job_command, SDP_JOB_SUBMIT, true},
    {"job", "status", sdp_job_command, SDP_JOB_STATUS},
    {"job", "cancel", sdp_job_command, SDP_JOB_CANCEL},
    {"job", "schedule-create", sdp_job_command, SDP_JOB_SCHEDULE_CREATE, true},
    {"job", "schedule-status", sdp_job_command, SDP_JOB_SCHEDULE_STATUS},
    {"job", "schedule-cancel", sdp_job_command, SDP_JOB_SCHEDULE_CANCEL},
    {"job", "capabilities", sdp_job_command, SDP_JOB_CAPABILITIES},
    {"dataset", "create", sdp_dataset_command, SDP_DATASET_CREATE, true},
    {"dataset", "get", sdp_dataset_command, SDP_DATASET_GET},
    {"dataset", "delete", sdp_dataset_command, SDP_DATASET_DELETE},
    {"dataset", "plsql-submit", sdp_dataset_command, SDP_DATASET_PLSQL_SUBMIT, true},
    {"model", "create", sdp_model_command, SDP_MODEL_CREATE, true},
    {"model", "status", sdp_model_command, SDP_MODEL_STATUS},
    {"model", "train", sdp_model_command, SDP_MODEL_TRAIN, true},
    {"model", "evaluate", sdp_model_command, SDP_MODEL_EVALUATE},
    {"model", "predict", sdp_model_command, SDP_MODEL_PREDICT},
    {"model", "activate", sdp_model_command, SDP_MODEL_ACTIVATE, true},
    {"model", "compare", sdp_model_command, SDP_MODEL_COMPARE},
    {"model", "checkpoint", sdp_model_command, SDP_MODEL_CHECKPOINT},
    {"model", "resume", sdp_model_command, SDP_MODEL_RESUME, true},
    {"model", "deactivate", sdp_model_command, SDP_MODEL_DEACTIVATE, true},
    {"model", "rollback", sdp_model_command, SDP_MODEL_ROLLBACK, true},
    {"model", "delete", sdp_model_command, SDP_MODEL_DELETE, true},
    {"model", "devices", sdp_model_command, SDP_MODEL_DEVICES},
    {"backup", "submit", sdp_backup_command, SDP_BACKUP_SUBMIT, true},
    {"backup", "status", sdp_backup_command, SDP_BACKUP_STATUS},
    {"backup", "cancel", sdp_backup_command, SDP_BACKUP_CANCEL},
    {"backup", "catalog", sdp_backup_command, SDP_BACKUP_CATALOG},
    {"backup", "verify", sdp_backup_command, SDP_BACKUP_VERIFY},
    {"backup", "restore", sdp_backup_command, SDP_BACKUP_RESTORE},
    {"backup", "delete", sdp_backup_command, SDP_BACKUP_DELETE},
    {"admin", "status", sdp_admin_command, SDP_ADMIN_STATUS},
    {"admin", "health", sdp_admin_command, SDP_ADMIN_HEALTH},
    {"admin", "health-check", sdp_admin_command, SDP_ADMIN_HEALTH_CHECK},
    {"admin", "config-preview", sdp_admin_command, SDP_ADMIN_CONFIG_PREVIEW},
    {"admin", "config-apply", sdp_admin_command, SDP_ADMIN_CONFIG_APPLY},
    {"admin", "support-preview", sdp_admin_command, SDP_ADMIN_SUPPORT_PREVIEW},
    {"admin", "support-create", sdp_admin_command, SDP_ADMIN_SUPPORT_CREATE},
    {"admin", "maintenance-submit", sdp_admin_command, SDP_ADMIN_MAINTENANCE_SUBMIT, true},
});
}

Reply Automation::management(std::string_view group, std::string_view operation, std::string_view arguments)
{
    if (group == "job" && operation == "watch") return watch(arguments);
    try
    {
        auto body = arguments.empty() ? Json::object() : Json::parse(arguments);
        if (!body.is_object()) return failure(SDP_INVALID_ARGUMENT, "Management arguments must be a JSON object");
        if (group == "sessions")
        {
            if (operation == "list" && body.empty()) return control("sessions");
            if (operation == "reset" && body.empty())
            {
                auto reply = invoke(sdp_session_reset, true);
                if (reply.error.code == SDP_OK) { active_ = false; autocommit_ = true; }
                return reply;
            }
            const auto id = body.at("session_id").get<std::string>();
            if (operation == "cancel") return invoke([&](auto handle, auto error) {
                return sdp_query_cancel(handle, text(id), body.value("request_id", uint32_t{}), error);
            }, true);
            if (operation == "terminate") return invoke([&](auto handle, auto error) {
                return sdp_session_terminate(handle, text(id), error);
            }, true);
        }
        for (const auto& method : rpc_methods)
        {
            if (method.group != group || method.name != operation) continue;
            const bool history_apply = group == "history" &&
                (operation == "restore" || operation == "revert") && body.contains("plan_id");
            if ((method.token || history_apply) && !body.contains("request_token"))
                body["request_token"] = request_token();
            const auto token = body.value("request_token", "");
            const auto payload = body.dump();
            return receipt(group, [&](auto handle, auto buffer, auto error) {
                return method.function(handle, method.method, text(payload), buffer, error);
            }, token);
        }
    }
    catch (const Json::exception&) { return failure(SDP_INVALID_ARGUMENT, "Invalid management JSON"); }
    return failure(SDP_INVALID_ARGUMENT, "Unknown management group or action");
}

Reply Automation::watch(std::string_view arguments)
{
    try
    {
        const auto body = Json::parse(arguments);
        if (!body.is_object() || body.size() != 1 || !body.contains("job_id") || !body.at("job_id").is_string())
            return failure(SDP_INVALID_ARGUMENT, "Job watch requires a job_id JSON field");
        const auto payload = body.dump();
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_);
        for (;;)
        {
            Buffer status;
            auto reply = invoke([&](auto handle, auto error) {
                return sdp_job_command(handle, SDP_JOB_STATUS, text(payload), &status.value, error);
            }, true);
            if (reply.error.code != SDP_OK) return reply;
            const auto state = status.json();
            const auto name = state.at("state").get<std::string>();
            if (name == "succeeded" || name == "failed" || name == "cancelled")
            {
                emit("job", state);
                if (name == "succeeded") return reply;
                if (name == "cancelled") return failure(SDP_CANCELLED, "Server job was cancelled");
                return failure(SDP_INVALID_STATE, "Server job failed");
            }
            if (Clock::now() >= deadline) return failure(SDP_TIMEOUT, "Timed out watching server job");
            if (WaitForSingleObject(terminal_.interrupt(), 100) == WAIT_OBJECT_0)
                return failure(SDP_CANCELLED, "Job watch cancelled");
        }
    }
    catch (const Json::exception&) { return failure(SDP_INVALID_ARGUMENT, "Invalid job watch JSON"); }
}

Reply Automation::password(std::string_view user, std::string_view old_password, std::string_view new_password)
{
    const auto token = request_token();
    auto reply = invoke([&](auto handle, auto error) {
        return sdp_password_set(handle, text(user), text(old_password), text(new_password), text(token), error);
    }, true);
    if (reply.error.unknown_outcome) reply.token = token;
    if (reply.error.code == SDP_OK) emit("password", Json{{"status", "changed"}});
    return reply;
}

void Automation::report(const Reply& reply, std::string_view source)
{
    const auto& error = reply.error;
    if (error.code == SDP_OK) return;
    Json data{{"code", error.code}, {"message", error.message}, {"sqlstate", error.sqlstate},
        {"line", error.line}, {"column", error.column}, {"unknown_outcome", error.unknown_outcome != 0}};
    if (!reply.token.empty()) data["request_token"] = reply.token;
    emit("error", data);
    std::cerr << terminal_text(error.message) << " (SDP " << error.code << ')';
    if (error.line && error.column) std::cerr << " at " << error.line << ':' << error.column;
    std::cerr << '\n';
    if (!reply.token.empty())
        std::cerr << "Outcome unknown. Reconnect and run \\outcome " << reply.token << '\n';

    if (!error.line || !error.column || error.column > 200 || source.empty()) return;
    auto words = inspect(source).leading;
    if (std::ranges::find(words, "USER") != words.end() ||
        std::ranges::find(words, "PASSWORD") != words.end()) return;
    for (uint32_t line = 1; line < error.line && !source.empty(); ++line)
    {
        const auto newline = source.find('\n');
        source = newline == source.npos ? std::string_view{} : source.substr(newline + 1);
    }
    if (source.empty()) return;
    source = source.substr(0, (std::min)(source.find('\n'), size_t{200}));
    const auto offset = (std::min)(static_cast<size_t>(error.column - 1), source.size());
    std::cerr << terminal_text(source) << '\n' << std::string(terminal_text(source.substr(0, offset)).size(), ' ') << "^\n";
}
}
