#pragma once

#include "connection.hpp"
#include "../automation/sql_script.hpp"

#include <ostream>

namespace cli
{
enum class Format { table, vertical, json, ndjson, csv };
Format parse_format(std::string_view name);

class Output
{
public:
    Output(std::ostream& stream, Format format);
    void emit(std::string_view kind, const Json& data);
    void finish(Exit status);
    void format(Format format);
    void flush() { stream_.flush(); }

private:
    void batch(const Json& data);
    std::ostream& stream_;
    Format format_;
    bool first_ = true;
    uint64_t row_ = 0;
    Json columns_ = Json::array();
};

struct Reply
{
    sdp_error error{};
    std::string token;
    bool sql{};
    Exit status() const { return exit_for(error, sql); }
};

class Automation
{
public:
    Automation(Connection& connection, const Terminal& terminal, Output& output, uint32_t timeout);
    void bind(std::string_view specification);
    void timeout(uint32_t value) { timeout_ = value; }
    void timing(bool value) { timing_ = value; }
    bool timing() const { return timing_; }
    bool active() const { return active_ && connection_.connected(); }
    bool autocommit() const { return autocommit_; }
    void set_atomic_script(bool value) { atomic_script_ = value; }
    Reply execute(std::string_view sql);
    Reply control(std::string_view command, std::string_view argument = {});
    Reply management(std::string_view group, std::string_view operation, std::string_view arguments);
    Reply password(std::string_view user, std::string_view old_password, std::string_view new_password);
    void report(const Reply& reply, std::string_view sql = {});

private:
    Reply sql(std::string_view sql, bool token, bool embedded_write);
    Reply stream(std::string_view result_id);
    Reply invoke(const Connection::Request& operation, bool sql = false);
    Reply receipt(std::string_view kind,
        const std::function<sdp_status(sdp_connection, sdp_buffer*, sdp_error*)>& operation,
        std::string_view token = {}, bool sql = true);
    Reply watch(std::string_view arguments);
    void emit(std::string_view kind, const Json& data);
    Connection& connection_;
    const Terminal& terminal_;
    Output& output_;
    uint32_t timeout_;
    Json bindings_ = Json::object();
    bool active_{};
    bool autocommit_ = true;
    bool timing_{};
    bool atomic_script_{};
};
}
