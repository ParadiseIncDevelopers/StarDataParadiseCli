#include "terminal.hpp"

#include <array>
#include <algorithm>
#include <atomic>
#include <cwctype>
#include <iostream>
#include <stdexcept>

namespace cli
{
namespace
{
std::atomic<HANDLE> ctrl_event;

BOOL WINAPI interrupt_handler(DWORD signal)
{
    if (signal != CTRL_C_EVENT && signal != CTRL_BREAK_EVENT) return FALSE;
    if (const auto event = ctrl_event.load()) SetEvent(event);
    return TRUE;
}

template<class T, size_t N>
struct ClearedArray
{
    std::array<T, N> data{};
    ~ClearedArray() { SecureZeroMemory(data.data(), sizeof(data)); }
};
}

std::string utf8(std::wstring_view value)
{
    if (value.empty()) return {};
    const auto length = static_cast<int>(value.size());
    const auto count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), length, nullptr, 0, nullptr, nullptr);
    if (count <= 0) throw std::invalid_argument("Invalid Unicode input");
    std::string result(count, '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), length, result.data(), count, nullptr, nullptr))
        throw std::invalid_argument("Invalid Unicode input");
    return result;
}

std::wstring wide(std::string_view value)
{
    if (value.empty()) return {};
    const auto length = static_cast<int>(value.size());
    const auto count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), length, nullptr, 0);
    if (count <= 0) throw std::invalid_argument("Invalid UTF-8 input");
    std::wstring result(count, L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), length, result.data(), count))
        throw std::invalid_argument("Invalid UTF-8 input");
    return result;
}

std::string terminal_text(std::string_view value)
{
    constexpr std::string_view digits = "0123456789abcdef";
    std::string result;
    for (const unsigned char byte : value)
    {
        if (byte >= 0x20 && byte <= 0x7e)
            result.push_back(static_cast<char>(byte));
        else
        {
            result += "\\x";
            result += digits[byte >> 4];
            result += digits[byte & 15];
        }
    }
    return result;
}

Terminal::Terminal()
    : input_(GetStdHandle(STD_INPUT_HANDLE)), output_(GetStdHandle(STD_OUTPUT_HANDLE)),
      interrupt_(CreateEventW(nullptr, TRUE, FALSE, nullptr))
{
    if (!interrupt_) throw std::runtime_error("Console interrupt unavailable");
    ctrl_event.store(interrupt_);
    if (!SetConsoleCtrlHandler(interrupt_handler, TRUE))
    {
        ctrl_event.store(nullptr);
        CloseHandle(interrupt_);
        throw std::runtime_error("Console interrupt handler unavailable");
    }
    console_input_ = GetConsoleMode(input_, &saved_input_) != 0;
    console_output_ = GetConsoleMode(output_, &saved_output_) != 0;
    if (console_input_) SetConsoleMode(input_, (saved_input_ | ENABLE_PROCESSED_INPUT) & ~ENABLE_ECHO_INPUT);
}

Terminal::~Terminal()
{
    SetConsoleCtrlHandler(interrupt_handler, FALSE);
    ctrl_event.store(nullptr);
    if (console_input_) SetConsoleMode(input_, saved_input_);
    if (console_output_) SetConsoleMode(output_, saved_output_);
    CloseHandle(interrupt_);
}

bool Terminal::interrupted() const noexcept
{
    return WaitForSingleObject(interrupt_, 0) == WAIT_OBJECT_0;
}

void Terminal::set_color(Color mode, bool json)
{
    if (json || mode == Color::never) return;
    if (mode == Color::automatic && (!console_output_ || GetEnvironmentVariableW(L"NO_COLOR", nullptr, 0)))
        return;
    color_ = !console_output_ || SetConsoleMode(output_, saved_output_ | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
}

void Terminal::prompt(bool connected, std::string_view detail) const
{
    if (!interactive() || quiet_input_) return;
    if (color_) std::cout << (connected ? "\033[32m" : "\033[36m");
    if (detail == "continue") std::cout << "    -> ";
    else if (connected) std::cout << "sdp(" << terminal_text(detail) << ")> ";
    else std::cout << "sdp(disconnected)> ";
    if (color_) std::cout << "\033[0m";
    std::cout << std::flush;
}

void Terminal::success(std::string_view message) const
{
    if (color_) std::cout << "\033[32m";
    std::cout << message;
    if (color_) std::cout << "\033[0m";
    std::cout << std::endl;
}

Line Terminal::line(std::string& output, size_t limit, bool secret, const std::vector<std::string>* history) const
{
    output.clear();
    if (secret) output.reserve(limit + 1);
    if (interrupted()) return Line::cancelled;
    if (console_input_) return console_line(output, limit, secret, history);
    return redirected_line(output, limit);
}

Line Terminal::console_line(std::string& output, size_t limit, bool secret, const std::vector<std::string>* history) const
{
    ClearedArray<wchar_t, 4096> storage;
    std::array<HANDLE, 2> sources{interrupt_, input_};
    size_t size{};
    bool overflow{};
    const bool echo = !secret && interactive() && !quiet_input_;
    std::wstring search_prefix;
    size_t history_pos = history ? history->size() : 0;
    bool browsing = false;
    const auto replace = [&](std::wstring_view next) {
        if (next.size() >= storage.data.size()) return;
        for (size_t index = 0; index < size; ++index) std::cout << "\b \b";
        std::cout.flush();
        std::ranges::copy(next, storage.data.begin());
        size = next.size();
        DWORD written{};
        if (size) WriteConsoleW(output_, storage.data.data(), static_cast<DWORD>(size), &written, nullptr);
    };
    while (true)
    {
        const auto wait = WaitForMultipleObjects(static_cast<DWORD>(sources.size()), sources.data(), FALSE, INFINITE);
        if (wait == WAIT_OBJECT_0) return Line::cancelled;
        if (wait != WAIT_OBJECT_0 + 1) return Line::failed;

        INPUT_RECORD record{};
        DWORD count{};
        if (!ReadConsoleInputW(input_, &record, 1, &count)) return Line::failed;
        if (!count || record.EventType != KEY_EVENT || !record.Event.KeyEvent.bKeyDown) continue;
        const auto key = record.Event.KeyEvent.wVirtualKeyCode;
        const auto c = record.Event.KeyEvent.uChar.UnicodeChar;
        const std::wstring_view input(storage.data.data(), size);
        if (echo && key == VK_TAB)
        {
            constexpr std::array commands{L"SELECT", L"INSERT", L"UPDATE", L"DELETE", L"CREATE", L"ALTER",
                L"DROP", L"BEGIN", L"DECLARE", L"CALL", L"COMMIT", L"ROLLBACK", L"SAVEPOINT", L"EXPLAIN",
                L"\\connect", L"\\status", L"\\capabilities", L"\\databases", L"\\tables", L"\\describe",
                L"\\format", L"\\timing", L"\\run", L"\\history", L"\\rpc", L"\\outcome", L"\\service",
                L"\\autocommit", L"\\begin", L"\\commit", L"\\rollback", L"\\savepoint", L"\\release",
                L"\\sessions", L"\\password", L"\\help", L"\\quit"};
            if (!input.empty() && input.find_first_of(L" \t") == input.npos)
            {
                for (const std::wstring_view command : commands)
                {
                    if (command.size() < input.size()) continue;
                    if (!std::equal(input.begin(), input.end(), command.begin(), [](wchar_t a, wchar_t b) {
                        return std::towupper(a) == std::towupper(b);
                    })) continue;
                    replace(std::wstring(command) + L" ");
                    break;
                }
            }
            continue;
        }
        const bool reverse_search = key == 'R' &&
            (record.Event.KeyEvent.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
        if (echo && history && (key == VK_UP || key == VK_DOWN || reverse_search))
        {
            if (!browsing) { search_prefix = input; history_pos = history->size(); browsing = true; }
            if (key == VK_UP || reverse_search)
            {
                for (size_t index = history_pos; index > 0; --index)
                {
                    const auto previous = wide(history->at(index - 1));
                    if (!previous.starts_with(search_prefix)) continue;
                    history_pos = index - 1;
                    replace(previous);
                    break;
                }
            }
            else
            {
                for (size_t index = history_pos + 1; index <= history->size(); ++index)
                {
                    const auto next = index == history->size() ? search_prefix : wide(history->at(index));
                    if (!next.starts_with(search_prefix)) continue;
                    history_pos = index;
                    replace(next);
                    break;
                }
            }
            continue;
        }
        if (c == L'\r')
        {
            if (echo) std::cout << '\n';
            if (overflow) return Line::too_long;
            try { output = utf8({storage.data.data(), size}); }
            catch (const std::invalid_argument&) { return Line::failed; }
            return output.size() <= limit ? Line::ready : Line::too_long;
        }
        if (c == L'\x1a' && size == 0) return Line::eof;
        if (c == L'\b' && size > 0)
        {
            storage.data[--size] = L'\0';
            browsing = false;
            if (echo) std::cout << "\b \b" << std::flush;
            continue;
        }
        if (c < 0x20 || c == 0x7f) continue;
        if (size == storage.data.size())
        {
            overflow = true;
            continue;
        }
        storage.data[size++] = c;
        browsing = false;
        if (echo)
        {
            DWORD written{};
            WriteConsoleW(output_, &c, 1, &written, nullptr);
        }
    }
}

Line Terminal::redirected_line(std::string& output, size_t limit) const
{
    if (!input_ || input_ == INVALID_HANDLE_VALUE) return Line::eof;
    const bool pipe = GetFileType(input_) == FILE_TYPE_PIPE;
    bool overflow{};
    bool eof{};
    while (!interrupted())
    {
        if (pipe)
        {
            DWORD available{};
            if (!PeekNamedPipe(input_, nullptr, 0, nullptr, &available, nullptr))
            {
                if (GetLastError() == ERROR_BROKEN_PIPE) { eof = true; break; }
                return Line::failed;
            }
            if (available == 0)
            {
                WaitForSingleObject(interrupt_, 20);
                continue;
            }
        }

        char byte{};
        DWORD read{};
        if (!ReadFile(input_, &byte, 1, &read, nullptr))
        {
            if (GetLastError() == ERROR_BROKEN_PIPE) { eof = true; break; }
            return Line::failed;
        }
        if (!read) { eof = true; break; }
        if (byte == '\n') break;
        if (output.size() < limit + 1) output.push_back(byte);
        else overflow = true;
        SecureZeroMemory(&byte, 1);
    }
    if (interrupted()) return Line::cancelled;
    if (!output.empty() && output.back() == '\r') output.pop_back();
    if (overflow || output.size() > limit) return Line::too_long;
    return output.empty() && eof ? Line::eof : Line::ready;
}
}
