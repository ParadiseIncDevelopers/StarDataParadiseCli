#pragma once

#include <windows.h>
#include <string>
#include <string_view>

namespace cli
{
enum class Color { automatic, always, never };
enum class Line { ready, eof, cancelled, failed, too_long };

struct Secret
{
    std::string bytes;
    ~Secret() { if (!bytes.empty()) SecureZeroMemory(bytes.data(), bytes.size()); }
};

std::string utf8(std::wstring_view value);
std::wstring wide(std::string_view value);
std::string terminal_text(std::string_view value);

class Terminal
{
public:
    Terminal();
    ~Terminal();
    Terminal(const Terminal&) = delete;
    Terminal& operator=(const Terminal&) = delete;

    bool interactive() const noexcept { return console_input_ && console_output_; }
    bool console_input() const noexcept { return console_input_; }
    HANDLE interrupt() const noexcept { return interrupt_; }
    bool interrupted() const noexcept;
    void set_color(Color mode, bool json);
    void prompt(bool connected) const;
    void success(std::string_view message) const;
    Line line(std::string& output, size_t limit, bool secret = false) const;

private:
    Line console_line(std::string& output, size_t limit, bool secret) const;
    Line redirected_line(std::string& output, size_t limit) const;

    HANDLE input_;
    HANDLE output_;
    HANDLE interrupt_;
    DWORD saved_input_{};
    DWORD saved_output_{};
    bool console_input_{};
    bool console_output_{};
    bool color_{};
};
}
