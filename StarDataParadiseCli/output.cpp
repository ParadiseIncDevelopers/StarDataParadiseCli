#include "automation.hpp"

namespace cli
{
namespace
{
std::string value_text(const Json& value)
{
    if (!value.is_object()) return value.is_string() ? value.get<std::string>() : value.dump();
    const auto type = value.value("t", "");
    if (type == "null") return R"(\N)";
    if (type == "text" || type == "i64" || type == "date" || type == "time" || type == "bool")
        return value_text(value.at("v"));
    if (type == "binary") return "0x" + value.at("hex").get<std::string>();
    if (type == "decimal")
    {
        auto coefficient = value.at("unscaled").get<std::string>();
        const bool negative = coefficient.starts_with('-');
        if (negative) coefficient.erase(0, 1);
        const auto scale = value.at("scale").get<size_t>();
        if (scale >= coefficient.size()) coefficient.insert(0, scale + 1 - coefficient.size(), '0');
        if (scale) coefficient.insert(coefficient.size() - scale, 1, '.');
        return negative ? "-" + coefficient : coefficient;
    }
    return value.dump();
}

void csv(std::ostream& output, std::string_view value, bool null)
{
    if (null) { output << "\\N"; return; }
    output << '"';
    for (const char c : value)
    {
        if (c == '"') output << '"';
        output << c;
    }
    output << '"';
}
}

Format parse_format(std::string_view name)
{
    if (name == "json") return Format::json;
    if (name == "ndjson") return Format::ndjson;
    if (name == "csv") return Format::csv;
    if (name == "table") return Format::table;
    if (name == "vertical") return Format::vertical;
    throw std::invalid_argument("Invalid output format");
}

Output::Output(std::ostream& stream, Format format) : stream_(stream), format_(format)
{
    if (format_ == Format::json) stream_ << "{\"events\":[";
}

void Output::format(Format format)
{
    if (format_ == Format::json || format == Format::json)
        throw std::invalid_argument("Use --format json before opening a JSON document");
    format_ = format;
}

void Output::emit(std::string_view kind, const Json& data)
{
    if (format_ == Format::json || format_ == Format::ndjson)
    {
        if (format_ == Format::json && !first_) stream_ << ',';
        stream_ << Json{{"type", kind}, {"data", data}}.dump();
        if (format_ == Format::ndjson) stream_ << '\n';
        first_ = false;
        return;
    }
    if (kind == "batch") { batch(data); return; }
    if (format_ == Format::csv || kind == "error") return;
    if (kind == "execute" && data.contains("result_id")) return;
    stream_ << terminal_text(data.dump()) << '\n';
}

void Output::batch(const Json& data)
{
    if (data.contains("columns") && data.at("columns").is_array())
    {
        columns_ = data.at("columns");
        row_ = 0;
        if (format_ != Format::vertical)
        {
            for (size_t index = 0; index < columns_.size(); ++index)
            {
                if (index) stream_ << (format_ == Format::csv ? ',' : '\t');
                const auto name = columns_[index].at("name").get<std::string>();
                if (format_ == Format::csv) csv(stream_, name, false);
                else stream_ << terminal_text(name);
            }
            stream_ << '\n';
        }
    }
    for (const auto& values : data.at("rows"))
    {
        if (format_ == Format::vertical) stream_ << "-[" << ++row_ << "]-\n";
        for (size_t index = 0; index < values.size(); ++index)
        {
            if (format_ == Format::vertical)
                stream_ << terminal_text(columns_.at(index).at("name").get<std::string>()) << " | ";
            else if (index) stream_ << (format_ == Format::csv ? ',' : '\t');
            const auto& value = values[index];
            const auto printed = value_text(value);
            if (format_ == Format::csv) csv(stream_, printed, value.is_null() || value.value("t", "") == "null");
            else stream_ << terminal_text(printed);
            if (format_ == Format::vertical) stream_ << '\n';
        }
        if (format_ != Format::vertical) stream_ << '\n';
    }
}

void Output::finish(Exit status)
{
    if (format_ == Format::json) stream_ << "],\"exit_code\":" << static_cast<int>(status) << "}\n";
    if (format_ == Format::ndjson) emit("status", Json{{"exit_code", static_cast<int>(status)}});
    stream_.flush();
    if (!stream_) throw std::runtime_error("Output could not be written");
}
}
