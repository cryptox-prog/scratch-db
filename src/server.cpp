#include "query/query_executor.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

std::atomic<bool> running = true;

struct ServerConfig {
    std::string host = "0.0.0.0";
    uint16_t port = 8080;
    std::filesystem::path data_root = "data";
};

struct HttpRequest {
    std::string method;
    std::string path;
    std::string body;
};

void handle_signal(int) {
    running = false;
}

std::string json_escape(const std::string& text) {
    std::string out;
    for (char ch : text) {
        switch (ch) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    out += "\\u00";
                    const char* hex = "0123456789abcdef";
                    out.push_back(hex[(ch >> 4) & 0x0f]);
                    out.push_back(hex[ch & 0x0f]);
                } else {
                    out.push_back(ch);
                }
                break;
        }
    }
    return out;
}

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

std::string query_for_executor(const std::string& text) {
    std::string query = trim(text);
    if (!query.empty() && query.back() == ';') {
        query.pop_back();
    }
    return trim(query);
}

std::string query_result_to_json(const QueryResult& result, const std::string& session_id) {
    std::ostringstream out;
    out << "{\"ok\":" << (result.ok() ? "true" : "false");
    out << ",\"session_id\":\"" << json_escape(session_id) << "\"";
    if (result.error.has_value()) {
        out << ",\"error\":{";
        out << "\"message\":\"" << json_escape(result.error->message) << "\"";
        out << ",\"token\":\"" << json_escape(result.error->token) << "\"";
        out << ",\"position\":" << result.error->position;
        out << "}";
    }

    out << ",\"columns\":[";
    for (std::size_t i = 0; i < result.columns.size(); ++i) {
        if (i > 0) {
            out << ",";
        }
        out << "{\"name\":\"" << json_escape(result.columns[i].name) << "\",";
        out << "\"type\":\"" << json_escape(result.columns[i].type) << "\"}";
    }
    out << "]";

    out << ",\"rows\":[";
    for (std::size_t i = 0; i < result.rows.size(); ++i) {
        if (i > 0) {
            out << ",";
        }
        out << "[";
        for (std::size_t j = 0; j < result.rows[i].size(); ++j) {
            if (j > 0) {
                out << ",";
            }
            out << "\"" << json_escape(result.rows[i][j]) << "\"";
        }
        out << "]";
    }
    out << "]";

    out << ",\"metadata\":{";
    out << "\"row_count\":" << result.metadata.row_count;
    out << ",\"message\":\"" << json_escape(result.metadata.message) << "\"";
    out << "}}";
    return out.str();
}

std::string http_response(int status, const std::string& status_text, const std::string& content_type, const std::string& body) {
    std::ostringstream out;
    out << "HTTP/1.1 " << status << " " << status_text << "\r\n";
    out << "Content-Type: " << content_type << "\r\n";
    out << "Content-Length: " << body.size() << "\r\n";
    out << "Connection: close\r\n";
    out << "Access-Control-Allow-Origin: *\r\n";
    out << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n";
    out << "Access-Control-Allow-Headers: Content-Type\r\n";
    out << "\r\n";
    out << body;
    return out.str();
}

bool send_all(int fd, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
#ifdef MSG_NOSIGNAL
        const ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
#else
        const ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, 0);
#endif
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

std::optional<std::size_t> content_length_from_headers(const std::string& headers) {
    std::istringstream in(headers);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const std::string lower_prefix = "content-length:";
        if (line.size() >= lower_prefix.size()) {
            std::string prefix = line.substr(0, lower_prefix.size());
            std::transform(prefix.begin(), prefix.end(), prefix.begin(), [](unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
            });
            if (prefix == lower_prefix) {
                std::string value = line.substr(lower_prefix.size());
                return static_cast<std::size_t>(std::stoull(value));
            }
        }
    }
    return std::nullopt;
}

std::optional<HttpRequest> read_request(int client_fd) {
    std::string buffer;
    char chunk[4096];
    std::size_t header_end = std::string::npos;
    while (header_end == std::string::npos && buffer.size() < 65536) {
        const ssize_t n = ::recv(client_fd, chunk, sizeof(chunk), 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return std::nullopt;
        }
        if (n == 0) {
            return std::nullopt;
        }
        buffer.append(chunk, static_cast<std::size_t>(n));
        header_end = buffer.find("\r\n\r\n");
    }
    if (header_end == std::string::npos) {
        return std::nullopt;
    }

    const std::string headers = buffer.substr(0, header_end);
    std::optional<std::size_t> content_length = content_length_from_headers(headers);
    const std::size_t body_start = header_end + 4;
    const std::size_t expected_size = body_start + content_length.value_or(0);
    while (buffer.size() < expected_size) {
        const ssize_t n = ::recv(client_fd, chunk, sizeof(chunk), 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return std::nullopt;
        }
        if (n == 0) {
            return std::nullopt;
        }
        buffer.append(chunk, static_cast<std::size_t>(n));
    }

    std::istringstream request_line(headers);
    HttpRequest request;
    if (!(request_line >> request.method >> request.path)) {
        return std::nullopt;
    }
    request.body = buffer.substr(body_start, content_length.value_or(0));
    return request;
}

std::optional<std::string> parse_json_string_field(const std::string& body, const std::string& field_name) {
    const std::string key = "\"" + field_name + "\"";
    std::size_t pos = body.find(key);
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    pos = body.find(':', pos + key.size());
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    pos = body.find('"', pos + 1);
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    ++pos;

    std::string value;
    bool escaping = false;
    for (; pos < body.size(); ++pos) {
        const char ch = body[pos];
        if (escaping) {
            if (ch == 'n') {
                value.push_back('\n');
            } else if (ch == 'r') {
                value.push_back('\r');
            } else if (ch == 't') {
                value.push_back('\t');
            } else {
                value.push_back(ch);
            }
            escaping = false;
            continue;
        }
        if (ch == '\\') {
            escaping = true;
            continue;
        }
        if (ch == '"') {
            return value;
        }
        value.push_back(ch);
    }
    return std::nullopt;
}

bool valid_session_id(const std::string& session_id) {
    if (session_id.empty() || session_id.size() > 64) {
        return false;
    }
    for (char ch : session_id) {
        if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_' && ch != '-') {
            return false;
        }
    }
    return true;
}

std::string index_html() {
    return R"(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Scratch DB</title>
<style>
body{font-family:system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;margin:0;background:#f7f7f8;color:#191919}
main{max-width:980px;margin:32px auto;padding:0 20px}
textarea{width:100%;min-height:170px;font:15px ui-monospace,SFMono-Regular,Menlo,monospace;padding:12px;border:1px solid #c9c9ce;border-radius:6px}
button{margin-top:10px;padding:9px 14px;border:1px solid #222;background:#222;color:white;border-radius:6px;cursor:pointer}
pre{background:#111;color:#f4f4f4;padding:16px;border-radius:6px;overflow:auto}
table{border-collapse:collapse;margin-top:16px;background:white}
th,td{border:1px solid #d8d8dd;padding:7px 10px;text-align:left}
</style>
</head>
<body>
<main>
<h1>Scratch DB</h1>
<textarea id="query">CREATE DATABASE demo;</textarea>
<br>
<button id="run">Run Query</button>
<pre id="raw"></pre>
<div id="table"></div>
</main>
<script>
function esc(s){return String(s).replace(/[&<>"']/g,c=>({"&":"&amp;","<":"&lt;",">":"&gt;","\"":"&quot;","'":"&#39;"}[c]));}
document.getElementById("run").onclick=async()=>{
  const query=document.getElementById("query").value;
  let session_id=localStorage.getItem("scratch_db_session_id");
  if(!session_id){session_id="web_"+Math.random().toString(16).slice(2)+Date.now().toString(16);localStorage.setItem("scratch_db_session_id",session_id);}
  const res=await fetch("/query",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({session_id,query})});
  const data=await res.json();
  document.getElementById("raw").textContent=JSON.stringify(data,null,2);
  if(!data.ok||!data.columns.length){document.getElementById("table").innerHTML="";return;}
  let html="<table><thead><tr>"+data.columns.map(c=>"<th>"+esc(c.name)+"</th>").join("")+"</tr></thead><tbody>";
  html+=data.rows.map(r=>"<tr>"+r.map(v=>"<td>"+esc(v)+"</td>").join("")+"</tr>").join("");
  html+="</tbody></table><p>"+data.metadata.row_count+" rows</p>";
  document.getElementById("table").innerHTML=html;
};
</script>
</body>
</html>)";
}

class HttpServer {
public:
    explicit HttpServer(ServerConfig config) : config_(std::move(config)) {}

    bool start() {
        server_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd_ < 0) {
            std::cerr << "socket failed: " << std::strerror(errno) << "\n";
            return false;
        }

        int reuse = 1;
        setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_port = htons(config_.port);
        if (::inet_pton(AF_INET, config_.host.c_str(), &address.sin_addr) != 1) {
            std::cerr << "invalid host: " << config_.host << "\n";
            ::close(server_fd_);
            server_fd_ = -1;
            return false;
        }
        if (::bind(server_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            std::cerr << "bind failed: " << std::strerror(errno) << "\n";
            ::close(server_fd_);
            server_fd_ = -1;
            return false;
        }
        if (::listen(server_fd_, 128) != 0) {
            std::cerr << "listen failed: " << std::strerror(errno) << "\n";
            ::close(server_fd_);
            server_fd_ = -1;
            return false;
        }

        std::cout << "scratch-db server listening on http://" << config_.host << ":" << config_.port << "\n";
        while (running) {
            sockaddr_in client_address {};
            socklen_t client_size = sizeof(client_address);
            const int client_fd = ::accept(server_fd_, reinterpret_cast<sockaddr*>(&client_address), &client_size);
            if (client_fd < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
            std::thread(&HttpServer::handle_client, this, client_fd).detach();
        }
        ::close(server_fd_);
        server_fd_ = -1;
        return true;
    }

private:
    struct Session {
        explicit Session(const std::filesystem::path& data_root) : executor(data_root) {}

        QueryExecutor executor;
        std::mutex mutex;
    };

    std::shared_ptr<Session> session_for(const std::string& session_id) {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        auto found = sessions_.find(session_id);
        if (found != sessions_.end()) {
            return found->second;
        }
        auto session = std::make_shared<Session>(config_.data_root);
        sessions_[session_id] = session;
        return session;
    }

    void handle_client(int client_fd) {
        const std::optional<HttpRequest> request = read_request(client_fd);
        if (!request.has_value()) {
            send_all(client_fd, http_response(400, "Bad Request", "application/json", "{\"ok\":false,\"error\":{\"message\":\"bad request\"}}"));
            ::close(client_fd);
            return;
        }

        if (request->method == "OPTIONS") {
            send_all(client_fd, http_response(204, "No Content", "text/plain", ""));
        } else if (request->method == "GET" && request->path == "/health") {
            send_all(client_fd, http_response(200, "OK", "application/json", "{\"ok\":true}"));
        } else if (request->method == "GET" && request->path == "/") {
            send_all(client_fd, http_response(200, "OK", "text/html; charset=utf-8", index_html()));
        } else if (request->method == "POST" && request->path == "/query") {
            const std::optional<std::string> query = parse_json_string_field(request->body, "query");
            if (!query.has_value()) {
                send_all(client_fd, http_response(400, "Bad Request", "application/json", "{\"ok\":false,\"error\":{\"message\":\"expected JSON body with query\"}}"));
            } else {
                std::string session_id = parse_json_string_field(request->body, "session_id").value_or("default");
                if (!valid_session_id(session_id)) {
                    send_all(client_fd, http_response(400, "Bad Request", "application/json", "{\"ok\":false,\"error\":{\"message\":\"invalid session_id\"}}"));
                    ::close(client_fd);
                    return;
                }
                std::shared_ptr<Session> session = session_for(session_id);
                QueryResult result;
                {
                    std::lock_guard<std::mutex> lock(session->mutex);
                    result = session->executor.execute(query_for_executor(*query));
                }
                send_all(client_fd, http_response(result.ok() ? 200 : 400, result.ok() ? "OK" : "Bad Request", "application/json", query_result_to_json(result, session_id)));
            }
        } else {
            send_all(client_fd, http_response(404, "Not Found", "application/json", "{\"ok\":false,\"error\":{\"message\":\"not found\"}}"));
        }

        ::close(client_fd);
    }

    ServerConfig config_;
    std::mutex sessions_mutex_;
    std::unordered_map<std::string, std::shared_ptr<Session>> sessions_;
    int server_fd_ = -1;
};

std::optional<uint16_t> parse_port(const std::string& text) {
    const long value = std::strtol(text.c_str(), nullptr, 10);
    if (value <= 0 || value > 65535) {
        return std::nullopt;
    }
    return static_cast<uint16_t>(value);
}

ServerConfig parse_args(int argc, char** argv) {
    ServerConfig config;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--host" && i + 1 < argc) {
            config.host = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            std::optional<uint16_t> port = parse_port(argv[++i]);
            if (port.has_value()) {
                config.port = *port;
            }
        } else if (arg == "--data" && i + 1 < argc) {
            config.data_root = argv[++i];
        }
    }
    return config;
}

}  // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);
    std::signal(SIGPIPE, SIG_IGN);

    HttpServer server(parse_args(argc, argv));
    if (!server.start()) {
        std::cerr << "failed to start scratch-db server\n";
        return 1;
    }
    return 0;
}
