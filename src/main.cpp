#include "cli/table_printer.hpp"
#include "query/query_executor.hpp"

#include <cctype>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>
#include <termios.h>
#include <unistd.h>

namespace {

std::string trim(const std::string& text) {
    std::size_t start = 0;
    while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start]))) {
        ++start;
    }

    std::size_t end = text.size();
    while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }

    return text.substr(start, end - start);
}

bool ends_with_semicolon(const std::string& text) {
    const std::string cleaned = trim(text);
    return !cleaned.empty() && cleaned.back() == ';';
}

std::string strip_semicolon(const std::string& text) {
    std::string result = trim(text);
    result.pop_back();
    return trim(result);
}

void print_result(const QueryResult& result) {
    if (result.error.has_value()) {
        std::cout << "error: " << result.error->message;
        if (!result.error->token.empty()) {
            std::cout << " [" << result.error->token << "]";
        }
        std::cout << "\n";
        if (!result.error->source.empty()) {
            const std::size_t position = std::min(result.error->position, result.error->source.size());
            const std::size_t line_start = result.error->source.rfind('\n', position == 0 ? 0 : position - 1);
            const std::size_t start = line_start == std::string::npos ? 0 : line_start + 1;
            const std::size_t line_end = result.error->source.find('\n', position);
            const std::size_t end = line_end == std::string::npos ? result.error->source.size() : line_end;
            const std::string line = result.error->source.substr(start, end - start);
            std::cout << line << "\n";
            std::cout << std::string(position - start, ' ') << "^\n";
        }
        return;
    }

    if (!result.metadata.message.empty()) {
        std::cout << result.metadata.message;
        std::cout << "\n";
    }

    if (result.columns.empty()) {
        return;
    }

    TablePrinter::print(std::cout, result);
}

class RawTerminal {
public:
    RawTerminal() {
        enabled_ = isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &original_) == 0;
        if (!enabled_) {
            return;
        }

        termios raw = original_;
        raw.c_lflag &= static_cast<unsigned int>(~(ECHO | ICANON));
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        enabled_ = tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == 0;
    }

    ~RawTerminal() {
        if (enabled_) {
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_);
        }
    }

    bool enabled() const {
        return enabled_;
    }

private:
    bool enabled_ = false;
    termios original_ {};
};

class LineEditor {
public:
    std::optional<std::string> read_line(const std::string& prompt) {
        if (!isatty(STDIN_FILENO)) {
            std::cout << prompt;
            std::string line;
            if (!std::getline(std::cin, line)) {
                return std::nullopt;
            }
            return line;
        }

        RawTerminal terminal;
        if (!terminal.enabled()) {
            std::cout << prompt;
            std::string line;
            if (!std::getline(std::cin, line)) {
                return std::nullopt;
            }
            return line;
        }

        std::string line;
        std::size_t cursor = 0;
        std::size_t history_index = history_.size();
        std::cout << prompt << std::flush;

        while (true) {
            char ch = '\0';
            if (read(STDIN_FILENO, &ch, 1) != 1) {
                return std::nullopt;
            }

            if (ch == '\n' || ch == '\r') {
                std::cout << "\n";
                if (!line.empty()) {
                    history_.push_back(line);
                }
                return line;
            }

            if (ch == 23 || ch == 8) {
                delete_previous_word(line, cursor);
                redraw(prompt, line, cursor);
                continue;
            }

            if (ch == 127 || ch == '\b') {
                if (cursor > 0) {
                    line.erase(cursor - 1, 1);
                    --cursor;
                    redraw(prompt, line, cursor);
                }
                continue;
            }

            if (ch == 27) {
                handle_escape(prompt, line, cursor, history_index);
                continue;
            }

            if (std::isprint(static_cast<unsigned char>(ch))) {
                line.insert(line.begin() + static_cast<std::ptrdiff_t>(cursor), ch);
                ++cursor;
                redraw(prompt, line, cursor);
            }
        }
    }

private:
    void handle_escape(
        const std::string& prompt,
        std::string& line,
        std::size_t& cursor,
        std::size_t& history_index
    ) {
        char open_bracket = '\0';
        if (read(STDIN_FILENO, &open_bracket, 1) != 1 || open_bracket != '[') {
            return;
        }

        std::string sequence;
        char code = '\0';
        if (read(STDIN_FILENO, &code, 1) != 1) {
            return;
        }
        sequence.push_back(code);

        if (code == 'D') {
            if (cursor > 0) {
                --cursor;
                std::cout << "\x1b[D" << std::flush;
            }
        } else if (code == 'C') {
            if (cursor < line.size()) {
                ++cursor;
                std::cout << "\x1b[C" << std::flush;
            }
        } else if (code == 'A') {
            if (history_index > 0) {
                --history_index;
                line = history_[history_index];
                cursor = line.size();
                redraw(prompt, line, cursor);
            }
        } else if (code == 'B') {
            if (history_index < history_.size()) {
                ++history_index;
                line = history_index == history_.size() ? "" : history_[history_index];
                cursor = line.size();
                redraw(prompt, line, cursor);
            }
        }

        while (!sequence.empty() && !std::isalpha(static_cast<unsigned char>(sequence.back())) && sequence.back() != '~') {
            if (read(STDIN_FILENO, &code, 1) != 1) {
                return;
            }
            sequence.push_back(code);
        }

        if (sequence == "1;5D" || sequence == "5D") {
            cursor = previous_word_start(line, cursor);
            redraw(prompt, line, cursor);
        } else if (sequence == "1;5C" || sequence == "5C") {
            cursor = next_word_start(line, cursor);
            redraw(prompt, line, cursor);
        } else if (sequence == "3;5~" || sequence == "3~") {
            delete_next_word(line, cursor);
            redraw(prompt, line, cursor);
        }
    }

    std::size_t previous_word_start(const std::string& line, std::size_t cursor) {
        while (cursor > 0 && std::isspace(static_cast<unsigned char>(line[cursor - 1]))) {
            --cursor;
        }
        while (cursor > 0 && !std::isspace(static_cast<unsigned char>(line[cursor - 1]))) {
            --cursor;
        }
        return cursor;
    }

    std::size_t next_word_start(const std::string& line, std::size_t cursor) {
        while (cursor < line.size() && !std::isspace(static_cast<unsigned char>(line[cursor]))) {
            ++cursor;
        }
        while (cursor < line.size() && std::isspace(static_cast<unsigned char>(line[cursor]))) {
            ++cursor;
        }
        return cursor;
    }

    std::size_t next_word_end(const std::string& line, std::size_t cursor) {
        while (cursor < line.size() && std::isspace(static_cast<unsigned char>(line[cursor]))) {
            ++cursor;
        }
        while (cursor < line.size() && !std::isspace(static_cast<unsigned char>(line[cursor]))) {
            ++cursor;
        }
        return cursor;
    }

    void delete_previous_word(std::string& line, std::size_t& cursor) {
        const std::size_t start = previous_word_start(line, cursor);
        line.erase(start, cursor - start);
        cursor = start;
    }

    void delete_next_word(std::string& line, std::size_t cursor) {
        const std::size_t end = next_word_end(line, cursor);
        line.erase(cursor, end - cursor);
    }

    void redraw(const std::string& prompt, const std::string& line, std::size_t cursor) {
        std::cout << "\r\x1b[2K" << prompt << line;
        if (line.size() > cursor) {
            std::cout << "\x1b[" << (line.size() - cursor) << "D";
        }
        std::cout << std::flush;
    }

    std::vector<std::string> history_;
};

class Cli {
public:
    explicit Cli(std::filesystem::path data_root) : executor_(std::move(data_root)) {}

    void run() {
        std::cout << "scratch-db\n";
        print_result(executor_.execute("HELP"));

        std::string statement;
        while (true) {
            std::optional<std::string> line = line_editor_.read_line(statement.empty() ? prompt() : "    -> ");
            if (!line.has_value()) {
                break;
            }

            if (trim(*line).empty()) {
                continue;
            }

            if (!statement.empty()) {
                statement += "\n";
            }
            statement += trim(*line);

            if (!ends_with_semicolon(statement)) {
                continue;
            }

            const std::string command = strip_semicolon(statement);
            statement.clear();
            if (command.empty()) {
                continue;
            }

            const QueryResult result = executor_.execute(command);
            print_result(result);
            if (result.should_exit) {
                break;
            }
        }
    }

private:
    std::string prompt() const {
        const std::string& current_database = executor_.current_database();
        return current_database.empty() ? "scratch-db> " : "scratch-db:" + current_database + "> ";
    }

    QueryExecutor executor_;
    LineEditor line_editor_;
};

}  // namespace

int main(int argc, char** argv) {
    const std::filesystem::path data_root = argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path("data");
    Cli cli(data_root);
    cli.run();
    return 0;
}
