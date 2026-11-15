#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

struct SqlScript
{
    std::vector<std::string_view> statements;
    size_t consumed{};
    std::string_view remainder;
    bool incomplete{};
};

// Views refer to the caller's input. The REPL can keep the unfinished suffix.
SqlScript split_sql(std::string_view input, bool end_of_input);
