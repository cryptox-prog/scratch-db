#include "database/database.hpp"
#include "test_utils.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

std::filesystem::path temp_path(const std::string& name) {
    return std::filesystem::path("/tmp") / ("scratch_db_server_" + name);
}

std::string json_escape(const std::string& text) {
    std::string out;
    for (char ch : text) {
        if (ch == '"' || ch == '\\') {
            out.push_back('\\');
        }
        if (ch == '\n') {
            out += "\\n";
        } else {
            out.push_back(ch);
        }
    }
    return out;
}

bool connect_socket(uint16_t port, int& fd) {
    fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1) {
        ::close(fd);
        return false;
    }
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(fd);
        fd = -1;
        return false;
    }
    timeval timeout {};
    timeout.tv_sec = 2;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    return true;
}

bool send_all(int fd, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t count = ::send(fd, data.data() + sent, data.size() - sent, 0);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (count == 0) {
            return false;
        }
        sent += static_cast<std::size_t>(count);
    }
    return true;
}

std::optional<std::string> http_post_query(uint16_t port, const std::string& session_id, const std::string& query) {
    int fd = -1;
    if (!connect_socket(port, fd)) {
        return std::nullopt;
    }

    const std::string body = "{\"session_id\":\"" + json_escape(session_id) + "\",\"query\":\"" + json_escape(query) + "\"}";
    std::ostringstream request;
    request << "POST /query HTTP/1.1\r\n";
    request << "Host: 127.0.0.1\r\n";
    request << "Content-Type: application/json\r\n";
    request << "Content-Length: " << body.size() << "\r\n";
    request << "Connection: close\r\n\r\n";
    request << body;
    if (!send_all(fd, request.str())) {
        ::close(fd);
        return std::nullopt;
    }

    std::string response;
    char buffer[4096];
    while (true) {
        const ssize_t count = ::recv(fd, buffer, sizeof(buffer), 0);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (count == 0) {
            break;
        }
        response.append(buffer, static_cast<std::size_t>(count));
    }
    ::close(fd);
    return response;
}

std::optional<std::string> query_body(uint16_t port, const std::string& session_id, const std::string& query) {
    std::optional<std::string> response = http_post_query(port, session_id, query);
    if (!response.has_value()) {
        return std::nullopt;
    }
    const std::size_t body = response->find("\r\n\r\n");
    if (body == std::string::npos) {
        return std::nullopt;
    }
    return response->substr(body + 4);
}

bool wait_for_server(uint16_t port) {
    for (int i = 0; i < 50; ++i) {
        int fd = -1;
        if (connect_socket(port, fd)) {
            ::close(fd);
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

std::filesystem::path executable_directory() {
    std::vector<char> buffer(4096, 0);
    const ssize_t size = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (size <= 0) {
        return ".";
    }
    return std::filesystem::path(std::string(buffer.data(), static_cast<std::size_t>(size))).parent_path();
}

pid_t start_server(uint16_t port, const std::filesystem::path& data_root) {
    const std::filesystem::path server_path = executable_directory() / "db_server";
    const std::string port_text = std::to_string(port);
    const std::string data_text = data_root.string();
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::execl(server_path.c_str(), server_path.c_str(), "--host", "127.0.0.1", "--port", port_text.c_str(), "--data", data_text.c_str(), nullptr);
        _exit(127);
    }
    return pid;
}

void stop_server(pid_t pid) {
    if (pid <= 0) {
        return;
    }
    ::kill(pid, SIGTERM);
    for (int i = 0; i < 50; ++i) {
        int status = 0;
        const pid_t done = ::waitpid(pid, &status, WNOHANG);
        if (done == pid) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ::kill(pid, SIGKILL);
    int status = 0;
    ::waitpid(pid, &status, 0);
}

class ServerProcess {
public:
    ServerProcess(uint16_t port, const std::filesystem::path& data_root) : pid_(start_server(port, data_root)) {}
    ~ServerProcess() {
        stop_server(pid_);
    }
    pid_t pid() const {
        return pid_;
    }

private:
    pid_t pid_ = -1;
};

std::string required_query_body(uint16_t port, const std::string& session_id, const std::string& query) {
    std::optional<std::string> body = query_body(port, session_id, query);
    require(body.has_value(), "query request failed: " + query);
    return *body;
}

void http_sessions_preserve_transaction_isolation() {
    const std::filesystem::path root = temp_path("transaction_isolation");
    std::filesystem::remove_all(root);
    const uint16_t port = 19081;
    {
        ServerProcess server(port, root);
        require(server.pid() > 0, "server fork failed");
        require(wait_for_server(port), "server did not start");

        require(required_query_body(port, "writer", "CREATE DATABASE db").find("\"ok\":true") != std::string::npos, "create database failed");
        require(required_query_body(port, "writer", "CREATE TABLE items (id INTEGER NOT NULL, name VARSTRING(16) NOT NULL)").find("\"ok\":true") != std::string::npos, "create table failed");
        require(required_query_body(port, "writer", "INSERT INTO items VALUES (1, 'alice')").find("\"ok\":true") != std::string::npos, "committed insert failed");
        require(required_query_body(port, "writer", "BEGIN").find("\"ok\":true") != std::string::npos, "begin failed");
        require(required_query_body(port, "writer", "INSERT INTO items VALUES (2, 'bob')").find("\"ok\":true") != std::string::npos, "transaction insert failed");

        require(required_query_body(port, "reader", "USE db").find("\"ok\":true") != std::string::npos, "reader use database failed");
        std::optional<std::string> blocked_reader_result = query_body(port, "reader", "SELECT * FROM items");
        require(!blocked_reader_result.has_value(), "reader completed while writer transaction was uncommitted");
        require(required_query_body(port, "writer", "ROLLBACK").find("\"ok\":true") != std::string::npos, "rollback failed");
    }

    Database recovered(root, "db");
    std::vector<TableRow> rows = recovered.scan_rows("items");
    require(rows.size() == 1, "rollback did not leave one committed row");
    require(rows[0].row.value(1)->string_data() == "alice", "rollback changed committed row");

    std::filesystem::remove_all(root);
}

void http_failed_transaction_requires_rollback() {
    const std::filesystem::path root = temp_path("transaction_abort");
    std::filesystem::remove_all(root);
    const uint16_t port = 19082;
    ServerProcess server(port, root);
    require(server.pid() > 0, "server fork failed");
    require(wait_for_server(port), "server did not start");

    require(required_query_body(port, "client", "CREATE DATABASE db").find("\"ok\":true") != std::string::npos, "create database failed");
    require(required_query_body(port, "client", "CREATE TABLE items (id INTEGER NOT NULL, name VARSTRING(16) NOT NULL)").find("\"ok\":true") != std::string::npos, "create table failed");
    require(required_query_body(port, "client", "CREATE UNIQUE INDEX idx_items_id ON items (id)").find("\"ok\":true") != std::string::npos, "create index failed");
    require(required_query_body(port, "client", "INSERT INTO items VALUES (1, 'alice')").find("\"ok\":true") != std::string::npos, "initial insert failed");
    require(required_query_body(port, "client", "BEGIN").find("\"ok\":true") != std::string::npos, "begin failed");
    require(required_query_body(port, "client", "INSERT INTO items VALUES (1, 'duplicate')").find("\"ok\":false") != std::string::npos, "duplicate insert did not fail");
    require(required_query_body(port, "client", "INSERT INTO items VALUES (2, 'bob')").find("\"ok\":false") != std::string::npos, "aborted transaction accepted write");
    require(required_query_body(port, "client", "COMMIT").find("\"ok\":false") != std::string::npos, "aborted transaction committed");
    require(required_query_body(port, "client", "ROLLBACK").find("\"ok\":true") != std::string::npos, "rollback after abort failed");

    const std::optional<std::string> rows = query_body(port, "client", "SELECT * FROM items");
    require(rows.has_value() && rows->find("\"row_count\":1") != std::string::npos, "failed transaction changed row count");
    require(rows->find("alice") != std::string::npos && rows->find("bob") == std::string::npos, "failed transaction changed visible rows");

    std::filesystem::remove_all(root);
}

}  // namespace

int main() {
    std::vector<TestCase> tests;
    tests.push_back({"server transaction isolation", http_sessions_preserve_transaction_isolation});
    tests.push_back({"server failed transaction rollback", http_failed_transaction_requires_rollback});
    return run_tests(tests);
}
