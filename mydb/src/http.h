// http.h — minimal single-threaded HTTP/1.1 server over raw POSIX sockets.
//
// ponytail: no keep-alive, no chunked transfer, no TLS, one request at a
// time (each connection closes after its response). That matches the
// storage engine's single-writer/no-locking design -- there is never a
// second in-flight request to race with. Good enough for one person
// clicking around a demo; a real deployment would want a thread pool
// (or at least keep-alive) and picking up an actual library becomes
// worth it at that point.
#pragma once
#include "json.h"
#include <arpa/inet.h>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <netinet/in.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace mydb {

struct HttpRequest {
    std::string method, path;
    std::map<std::string, std::string> headers;
    std::map<std::string, std::string> params; // path params (":name" segments)
    std::map<std::string, std::string> query;  // ?key=value
    std::string body;

    JsonValue json() const { return body.empty() ? JsonValue::object() : parse_json(body); }
};

struct HttpResponse {
    int status = 200;
    std::string content_type = "application/json";
    std::string body;
    static HttpResponse json_ok(const JsonValue& v) { HttpResponse r; r.body = v.dump(); return r; }
    static HttpResponse json_error(int status, const std::string& msg) {
        HttpResponse r; r.status = status;
        JsonValue v = JsonValue::object(); v.set("error", JsonValue::string(msg));
        r.body = v.dump();
        return r;
    }
};

inline std::string url_decode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int code = std::stoi(s.substr(i + 1, 2), nullptr, 16);
            out += static_cast<char>(code);
            i += 2;
        } else if (s[i] == '+') out += ' ';
        else out += s[i];
    }
    return out;
}

class HttpServer {
public:
    using Handler = std::function<HttpResponse(const HttpRequest&)>;

    void route(const std::string& method, const std::string& pattern, Handler h) {
        routes_.push_back({method, split_path(pattern), h});
    }
    void get(const std::string& p, Handler h) { route("GET", p, h); }
    void post(const std::string& p, Handler h) { route("POST", p, h); }
    void patch_(const std::string& p, Handler h) { route("PATCH", p, h); }
    void del(const std::string& p, Handler h) { route("DELETE", p, h); }

    void serve_static(const std::string& dir) { static_dir_ = dir; }

    void listen(int port) {
        int server_fd = socket(AF_INET, SOCK_STREAM, 0);
        int opt = 1;
        setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);
        if (bind(server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            std::cerr << "http: bind failed on port " << port << "\n"; std::exit(1);
        }
        if (::listen(server_fd, 16) < 0) { std::cerr << "http: listen failed\n"; std::exit(1); }
        std::cout << "mydb server listening on http://localhost:" << port << "\n";
        while (true) {
            int client = accept(server_fd, nullptr, nullptr);
            if (client < 0) continue;
            handle_connection(client);
            close(client);
        }
    }

private:
    struct Route { std::string method; std::vector<std::string> segments; Handler handler; };
    std::vector<Route> routes_;
    std::string static_dir_;

    static std::vector<std::string> split_path(const std::string& path) {
        std::vector<std::string> out;
        std::string cur;
        for (char c : path) {
            if (c == '/') { if (!cur.empty()) out.push_back(cur); cur.clear(); }
            else cur += c;
        }
        if (!cur.empty()) out.push_back(cur);
        return out;
    }

    static bool match(const std::vector<std::string>& pattern, const std::vector<std::string>& actual,
                       std::map<std::string, std::string>& params) {
        if (pattern.size() != actual.size()) return false;
        for (size_t i = 0; i < pattern.size(); i++) {
            if (!pattern[i].empty() && pattern[i][0] == ':') params[pattern[i].substr(1)] = actual[i];
            else if (pattern[i] != actual[i]) return false;
        }
        return true;
    }

    static std::string content_type_for(const std::string& path) {
        auto ends_with = [&](const char* suf) { size_t n = std::strlen(suf); return path.size() >= n && path.compare(path.size() - n, n, suf) == 0; };
        if (ends_with(".html")) return "text/html";
        if (ends_with(".js")) return "application/javascript";
        if (ends_with(".css")) return "text/css";
        if (ends_with(".json")) return "application/json";
        if (ends_with(".md")) return "text/markdown";
        return "application/octet-stream";
    }

    bool try_static(const std::string& req_path, HttpResponse& out) {
        if (static_dir_.empty()) return false;
        std::string rel = req_path == "/" ? "/index.html" : req_path;
        std::string full = static_dir_ + rel;
        std::ifstream f(full, std::ios::binary);
        if (!f.good()) return false;
        std::ostringstream ss; ss << f.rdbuf();
        out.status = 200; out.body = ss.str(); out.content_type = content_type_for(full);
        return true;
    }

    void handle_connection(int client) {
        std::string raw = read_request_raw(client);
        if (raw.empty()) return;
        HttpRequest req;
        HttpResponse resp;
        try {
            parse_request(raw, client, req);
            dispatch(req, resp);
        } catch (const std::exception& e) {
            resp = HttpResponse::json_error(500, e.what());
        }
        write_response(client, resp);
    }

    std::string read_request_raw(int client) {
        std::string data;
        char buf[4096];
        // read headers until blank line
        while (data.find("\r\n\r\n") == std::string::npos) {
            ssize_t n = recv(client, buf, sizeof(buf), 0);
            if (n <= 0) return data;
            data.append(buf, n);
        }
        size_t header_end = data.find("\r\n\r\n") + 4;
        // find content-length to know how much more body to read
        size_t content_length = 0;
        {
            size_t pos = data.find("Content-Length:");
            if (pos == std::string::npos) pos = data.find("content-length:");
            if (pos != std::string::npos) {
                size_t line_end = data.find("\r\n", pos);
                std::string val = data.substr(pos, line_end - pos);
                size_t colon = val.find(':');
                content_length = std::stoul(val.substr(colon + 1));
            }
        }
        size_t have_body = data.size() - header_end;
        while (have_body < content_length) {
            ssize_t n = recv(client, buf, sizeof(buf), 0);
            if (n <= 0) break;
            data.append(buf, n);
            have_body += n;
        }
        return data;
    }

    void parse_request(const std::string& raw, int /*client*/, HttpRequest& req) {
        size_t header_end = raw.find("\r\n\r\n");
        std::string head = raw.substr(0, header_end);
        req.body = header_end == std::string::npos ? "" : raw.substr(header_end + 4);

        std::istringstream hs(head);
        std::string line;
        std::getline(hs, line);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream ls(line);
        std::string full_path;
        ls >> req.method >> full_path;

        size_t qpos = full_path.find('?');
        std::string path = qpos == std::string::npos ? full_path : full_path.substr(0, qpos);
        req.path = url_decode(path);
        if (qpos != std::string::npos) {
            std::string qs = full_path.substr(qpos + 1);
            std::istringstream qss(qs);
            std::string kv;
            while (std::getline(qss, kv, '&')) {
                size_t eq = kv.find('=');
                if (eq == std::string::npos) req.query[url_decode(kv)] = "";
                else req.query[url_decode(kv.substr(0, eq))] = url_decode(kv.substr(eq + 1));
            }
        }
        while (std::getline(hs, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            size_t colon = line.find(':');
            if (colon == std::string::npos) continue;
            std::string key = line.substr(0, colon);
            std::string val = line.substr(colon + 1);
            while (!val.empty() && val.front() == ' ') val.erase(0, 1);
            req.headers[key] = val;
        }
    }

    void dispatch(HttpRequest& req, HttpResponse& resp) {
        auto segs = split_path(req.path);
        for (auto& r : routes_) {
            if (r.method != req.method) continue;
            std::map<std::string, std::string> params;
            if (match(r.segments, segs, params)) {
                req.params = params;
                resp = r.handler(req);
                return;
            }
        }
        if (req.method == "GET" && try_static(req.path, resp)) return;
        resp = HttpResponse::json_error(404, "not found: " + req.method + " " + req.path);
    }

    void write_response(int client, const HttpResponse& resp) {
        static const std::map<int, std::string> reasons = {
            {200, "OK"}, {201, "Created"}, {400, "Bad Request"}, {404, "Not Found"}, {500, "Internal Server Error"}};
        std::string reason = reasons.count(resp.status) ? reasons.at(resp.status) : "OK";
        std::ostringstream os;
        os << "HTTP/1.1 " << resp.status << " " << reason << "\r\n"
           << "Content-Type: " << resp.content_type << "; charset=utf-8\r\n"
           << "Content-Length: " << resp.body.size() << "\r\n"
           << "Connection: close\r\n"
           << "Access-Control-Allow-Origin: *\r\n"
           << "\r\n"
           << resp.body;
        std::string out = os.str();
        send(client, out.data(), out.size(), 0);
    }
};

} // namespace mydb
