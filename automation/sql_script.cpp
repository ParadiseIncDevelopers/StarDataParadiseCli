#include "sql_script.hpp"

#include <algorithm>
#include <cctype>

namespace
{
bool identifier(unsigned char c)
{
    return c >= 128 || std::isalnum(c) || c == '_';
}

std::string keyword(std::string_view value)
{
    std::string upper(value);
    for (char& c : upper)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return upper;
}

struct Boundary
{
    bool pl{};
    bool begun{};
    bool just_closed{};
    std::string closed;
    std::vector<std::string> prefix;
    std::vector<std::string> blocks;

    void word(const std::string& token)
    {
        if (prefix.size() < 5) prefix.push_back(token);
        if (!pl)
        {
            if (prefix.size() == 1 && (token == "DECLARE" || token == "BEGIN")) pl = true;
            else if (prefix.front() == "CREATE" && (prefix.size() == 2 ||
                (prefix.size() == 4 && prefix[1] == "OR" && prefix[2] == "REPLACE")))
            {
                pl = token == "PROCEDURE" || token == "FUNCTION" || token == "TRIGGER";
            }
        }
        else if (prefix.size() == 2 && prefix[0] == "BEGIN" &&
            (token == "TRANSACTION" || token == "WORK"))
        {
            pl = false;
            blocks.clear();
            return;
        }
        if (!pl) return;
        if (just_closed)
        {
            just_closed = false;
            if (token == closed && token != "BEGIN") return;
        }
        if (token == "BEGIN")
        {
            begun = true;
            blocks.push_back(token);
        }
        else if (begun && (token == "IF" || token == "CASE" || token == "LOOP"))
        {
            blocks.push_back(token);
        }
        else if (token == "END" && !blocks.empty())
        {
            closed = std::move(blocks.back());
            blocks.pop_back();
            just_closed = true;
        }
    }

    bool complete() const
    {
        return !pl || (begun && blocks.empty()) || (prefix.size() == 1 && prefix[0] == "BEGIN");
    }
};
}

SqlScript split_sql(std::string_view input, bool end_of_input)
{
    SqlScript output;
    Boundary boundary;
    size_t start = 0;
    size_t position = 0;
    bool has_code = false;
    bool open = false;
    while (position < input.size())
    {
        char c = input[position];
        if (std::isspace(static_cast<unsigned char>(c))) { ++position; continue; }
        if (input.substr(position, 2) == "--")
        {
            const auto newline = input.find('\n', position + 2);
            position = newline == std::string_view::npos ? input.size() : newline + 1;
            continue;
        }
        if (input.substr(position, 2) == "/*")
        {
            const auto close = input.find("*/", position + 2);
            if (close == std::string_view::npos) { open = true; break; }
            position = close + 2;
            continue;
        }
        if (c == '\'' || c == '"')
        {
            has_code = true;
            boundary.just_closed = false;
            open = true;
            ++position;
            while (position < input.size())
            {
                if (input[position++] != c) continue;
                if (position < input.size() && input[position] == c) { ++position; continue; }
                open = false;
                break;
            }
            if (open) break;
            continue;
        }
        if (identifier(static_cast<unsigned char>(c)))
        {
            const auto from = position++;
            while (position < input.size() && identifier(static_cast<unsigned char>(input[position]))) ++position;
            boundary.word(keyword(input.substr(from, position - from)));
            has_code = true;
            continue;
        }
        ++position;
        if (c == ';')
        {
            if (boundary.complete())
            {
                if (has_code) output.statements.push_back(input.substr(start, position - start));
                start = position;
                boundary = Boundary{};
                has_code = false;
            }
        }
        else has_code = true;
        boundary.just_closed = false;
    }
    if (!open && (!has_code || (end_of_input && boundary.complete())))
    {
        if (has_code) output.statements.push_back(input.substr(start));
        start = input.size();
    }
    output.consumed = start;
    output.remainder = input.substr(start);
    output.incomplete = open || (has_code && !boundary.complete());
    return output;
}
