#include "http_server.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <map>
#include <sstream>

namespace {

using Socket = SOCKET;

struct Request {
    std::string method;
    std::string target;
    std::string body;
};

std::string urlDecode(std::string value) {
    std::string result;
    result.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            unsigned int code = 0;
            const auto hex = [](char ch) -> int {
                if (ch >= '0' && ch <= '9') return ch - '0';
                if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
                if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
                return -1;
            };
            const int high = hex(value[i + 1]);
            const int low = hex(value[i + 2]);
            if (high >= 0 && low >= 0) {
                result.push_back(static_cast<char>((high << 4) | low));
                i += 2;
                continue;
            }
        }
        if (value[i] == '+') result.push_back(' ');
        else result.push_back(value[i]);
    }
    return result;
}

std::map<std::string, std::string> queryParams(const std::string &target) {
    std::map<std::string, std::string> result;
    const size_t question = target.find('?');
    if (question == std::string::npos) return result;
    std::string query = target.substr(question + 1);
    while (!query.empty()) {
        const size_t amp = query.find('&');
        const std::string item = query.substr(0, amp);
        const size_t equal = item.find('=');
        const std::string key = equal == std::string::npos ? item : item.substr(0, equal);
        const std::string value = equal == std::string::npos ? "" : item.substr(equal + 1);
        result[urlDecode(key)] = urlDecode(value);
        if (amp == std::string::npos) break;
        query.erase(0, amp + 1);
    }
    return result;
}

std::string pathOnly(const std::string &target) {
    const size_t question = target.find('?');
    return target.substr(0, question == std::string::npos ? target.size() : question);
}

bool parseRequest(const std::string &raw, Request &request, size_t &headerEnd) {
    headerEnd = raw.find("\r\n\r\n");
    if (headerEnd == std::string::npos) return false;
    const size_t firstLineEnd = raw.find("\r\n");
    if (firstLineEnd == std::string::npos || firstLineEnd > headerEnd) return false;
    std::istringstream first(raw.substr(0, firstLineEnd));
    std::string version;
    if (!(first >> request.method >> request.target >> version)) return false;
    return version == "HTTP/1.1" || version == "HTTP/1.0";
}

int contentLength(const std::string &raw, size_t headerEnd) {
    size_t lineStart = raw.find("\r\n") + 2;
    while (lineStart < headerEnd) {
        const size_t lineEnd = raw.find("\r\n", lineStart);
        if (lineEnd == std::string::npos || lineEnd > headerEnd) break;
        const std::string line = raw.substr(lineStart, lineEnd - lineStart);
        const size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string key = line.substr(0, colon);
            std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (key == "content-length") {
                const char *begin = line.c_str() + colon + 1;
                while (*begin == ' ' || *begin == '\t') ++begin;
                const int value = std::atoi(begin);
                return std::clamp(value, 0, 1024 * 1024);
            }
        }
        lineStart = lineEnd + 2;
    }
    return 0;
}

std::string jsonArray(const std::vector<HubTask> &tasks) {
    std::string result = "[";
    for (size_t i = 0; i < tasks.size(); ++i) {
        if (i) result += ',';
        result += taskToJson(tasks[i]);
    }
    return result + ']';
}

std::string responseBody(const std::string &body, int status, const std::string &statusText) {
    return "HTTP/1.1 " + std::to_string(status) + " " + statusText + "\r\n"
           "Content-Type: application/json; charset=utf-8\r\n"
           "Access-Control-Allow-Origin: *\r\n"
           "Access-Control-Allow-Headers: Content-Type, Authorization\r\n"
           "Access-Control-Allow-Methods: GET, POST, DELETE, OPTIONS\r\n"
           "Cache-Control: no-store\r\n"
           "Connection: close\r\n"
           "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

std::string numberQuery(const std::map<std::string, std::string> &query, const char *name, int fallback, int minimum, int maximum) {
    const auto it = query.find(name);
    if (it == query.end()) return std::to_string(fallback);
    int value = fallback;
    std::from_chars(it->second.data(), it->second.data() + it->second.size(), value);
    return std::to_string(std::clamp(value, minimum, maximum));
}

std::wstring queryWide(const std::map<std::string, std::string> &query, const char *name) {
    const auto it = query.find(name);
    return it == query.end() ? L"" : jsonlite::fromUtf8(it->second);
}

std::string jsonError(const char *detail) {
    return std::string("{\"detail\":\"") + detail + "\"}";
}

std::string integrationResultJson(const IntegrationResult &result) {
    return std::string("{\"success\":") + (result.success ? "true" : "false") +
           ",\"changed\":" + (result.changed ? "true" : "false") +
           ",\"forwardTarget\":" + (result.forwardTarget ? "true" : "false") +
           ",\"message\":\"" + jsonlite::escapeUtf8(result.message) + "\"" +
           (result.success ? "" : ",\"error\":\"" + jsonlite::escapeUtf8(result.message) + "\"") + "}";
}

} // namespace

HttpServer::HttpServer(TaskStore &store, IntegrationManager &integrations)
    : store_(store), integrations_(integrations) {}

HttpServer::~HttpServer() { stop(); }

bool HttpServer::start(unsigned short port) {
    if (running_) return true;
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return false;
    const Socket listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) { WSACleanup(); return false; }
    BOOL reuse = TRUE;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuse), sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == SOCKET_ERROR || listen(listener, 8) == SOCKET_ERROR) {
        closesocket(listener); WSACleanup(); return false;
    }
    listener_ = static_cast<std::uintptr_t>(listener);
    port_ = port;
    stopping_ = false;
    running_ = true;
    worker_ = std::thread(&HttpServer::run, this);
    return true;
}

void HttpServer::stop() {
    if (!running_ && listener_ == 0) return;
    stopping_ = true;
    if (listener_ != 0) {
        const Socket listener = static_cast<Socket>(listener_);
        shutdown(listener, SD_BOTH);
        closesocket(listener);
        listener_ = 0;
    }
    if (worker_.joinable()) worker_.join();
    running_ = false;
    WSACleanup();
}

void HttpServer::run() {
    const Socket listener = static_cast<Socket>(listener_);
    while (!stopping_) {
        sockaddr_in clientAddress{};
        int addressLength = sizeof(clientAddress);
        const Socket client = accept(listener, reinterpret_cast<sockaddr *>(&clientAddress), &addressLength);
        if (client == INVALID_SOCKET) break;
        const DWORD timeout = 2000;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout), sizeof(timeout));
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&timeout), sizeof(timeout));
        handleClient(static_cast<std::uintptr_t>(client));
        closesocket(client);
    }
    running_ = false;
}

void HttpServer::handleClient(std::uintptr_t rawSocket) {
    const Socket client = static_cast<Socket>(rawSocket);
    std::string raw;
    raw.reserve(4096);
    char buffer[8192];
    size_t headerEnd = std::string::npos;
    int bodyLength = 0;
    while (raw.size() < 1024 * 1024 + 8192) {
        const int received = recv(client, buffer, sizeof(buffer), 0);
        if (received <= 0) return;
        raw.append(buffer, static_cast<size_t>(received));
        if (headerEnd == std::string::npos) {
            Request headerRequest;
            if (parseRequest(raw, headerRequest, headerEnd)) {
                bodyLength = contentLength(raw, headerEnd);
                const size_t bodyStart = headerEnd + 4;
                if (raw.size() >= bodyStart + static_cast<size_t>(bodyLength)) break;
            }
        }
        if (headerEnd != std::string::npos && raw.size() >= headerEnd + 4 + static_cast<size_t>(bodyLength)) break;
    }
    Request request;
    if (!parseRequest(raw, request, headerEnd)) return;
    const size_t bodyStart = headerEnd + 4;
    const size_t expected = std::min(raw.size(), bodyStart + static_cast<size_t>(bodyLength));
    request.body = raw.substr(bodyStart, expected - bodyStart);
    int status = 200;
    std::string statusText = "OK";
    const std::string body = route(request.method, request.target, request.body, status, statusText);
    const std::string response = responseBody(body, status, statusText);
    size_t sent = 0;
    while (sent < response.size()) {
        const int count = send(client, response.data() + sent, static_cast<int>(response.size() - sent), 0);
        if (count <= 0) break;
        sent += static_cast<size_t>(count);
    }
}

std::string HttpServer::route(const std::string &method, const std::string &target,
                              const std::string &body, int &status, std::string &statusText) {
    if (method == "OPTIONS") { status = 204; statusText = "No Content"; return "{}"; }
    const std::string path = pathOnly(target);
    const auto query = queryParams(target);
    if (method == "GET" && path == "/api/health") {
        return std::string("{\"status\":\"") + (store_.ready() ? "ok" : "degraded") + "\",\"service\":\"AI Task Hub Win32\",\"version\":\"0.3.4-win32\"}";
    }
    if (method == "GET" && path == "/api/status") {
        const auto snapshot = store_.snapshot(1);
        return std::string("{\"status\":\"") + (store_.ready() ? "ok" : "degraded") + "\",\"version\":\"0.3.4-win32\",\"runtime\":\"C++/Win32/Direct2D\",\"db\":{\"ok\":" + (store_.ready() ? "true" : "false") + ",\"backend\":\"sqlite\",\"database\":\"" + jsonlite::escapeUtf8(store_.databasePath()) + "\"},\"tasks\":" + std::to_string(snapshot.counts.total) + "}";
    }
    if (method == "GET" && path == "/api/integrations/status") {
        return integrations_.statusJson();
    }
    if (method == "POST" && path == "/api/integrations/chatgpt/heartbeat") {
        if (!integrations_.recordChatGptHeartbeat(body)) {
            status = 400;
            statusText = "Bad Request";
            return jsonError("invalid heartbeat");
        }
        return "{\"success\":true}";
    }
    // 接入配置只允许由本机 UI 修改，不向任意网页开放安装接口。
    if (method == "GET" && path == "/api/tasks/summary") {
        return "{\"counts\":" + countsToJson(store_.snapshot(1).counts) + "}";
    }
    if (method == "GET" && path == "/api/tasks/snapshot") {
        const int limit = std::stoi(numberQuery(query, "limit", 100, 1, 500));
        const auto snapshot = store_.snapshot(limit);
        const auto bucket = [&](const wchar_t *status) {
            bool more = false;
            const auto items = store_.tasks(L"", status, {}, {}, limit, 0, &more);
            return "{\"tasks\":" + jsonArray(items) + ",\"hasMore\":" + (more ? "true" : "false") + "}";
        };
        std::string buckets = "{\"RUNNING\":" + bucket(L"RUNNING") + ",";
        buckets += "\"NEEDS_INPUT\":" + bucket(L"NEEDS_INPUT") + ",";
        buckets += "\"COMPLETED_UNREAD\":" + bucket(L"COMPLETED_UNREAD") + ",";
        buckets += "\"FAILED_UNREAD\":" + bucket(L"FAILED_UNREAD") + ",";
        buckets += "\"VIEWED\":" + bucket(L"VIEWED") + ",";
        buckets += "\"IGNORED\":" + bucket(L"IGNORED") + "}";
        return "{\"counts\":" + countsToJson(snapshot.counts) + ",\"buckets\":" + buckets + "}";
    }
    if (method == "GET" && path == "/api/tasks") {
        const int limit = std::stoi(numberQuery(query, "limit", 200, 1, 500));
        const int offset = std::stoi(numberQuery(query, "offset", 0, 0, 1'000'000));
        const std::wstring view = queryWide(query, "view");
        const std::wstring statusFilter = queryWide(query, "status");
        bool more = false;
        const auto result = store_.tasks(view, statusFilter, queryWide(query, "source"), queryWide(query, "search"), limit, offset, &more);
        return "{\"tasks\":" + jsonArray(result) + ",\"hasMore\":" + (more ? "true" : "false") + "}";
    }
    if (method == "POST" && path == "/api/events") {
        jsonlite::Value event;
        std::string error;
        if (!jsonlite::parseUtf8(body, event, error) || !event.isObject()) { status = 400; statusText = "Bad Request"; return jsonError("invalid event JSON"); }
        const auto id = store_.ingest(event);
        if (id <= 0) { status = 422; statusText = "Unprocessable Entity"; return "{\"detail\":\"" + jsonlite::escapeUtf8(store_.error()) + "\"}"; }
        HubTask task;
        store_.task(id, task);
        return "{\"success\":true,\"taskId\":" + std::to_string(id) + ",\"task\":" + taskToJson(task) + "}";
    }
    if (method == "POST" && path == "/api/tasks/read-all") {
        return "{\"success\":true,\"count\":" + std::to_string(store_.markAllViewed()) + "}";
    }
    const std::string prefix = "/api/tasks/";
    if (path.rfind(prefix, 0) == 0) {
        const std::string remainder = path.substr(prefix.size());
        const size_t slash = remainder.find('/');
        const std::string idText = remainder.substr(0, slash);
        std::int64_t id = 0;
        std::from_chars(idText.data(), idText.data() + idText.size(), id);
        const std::string action = slash == std::string::npos ? "" : remainder.substr(slash + 1);
        HubTask task;
        if (method == "GET" && action.empty()) {
            if (!store_.task(id, task)) { status = 404; statusText = "Not Found"; return jsonError("task not found"); }
            return "{\"task\":" + taskToJson(task) + "}";
        }
        if (method == "GET" && action == "events") {
            if (!store_.task(id, task)) { status = 404; statusText = "Not Found"; return jsonError("task not found"); }
            const auto allEvents = store_.events(id);
            std::string list = "[";
            for (size_t i = 0; i < allEvents.size(); ++i) { if (i) list += ','; list += eventToJson(allEvents[i]); }
            return "{\"events\":" + list + "}";
        }
        if (method == "GET" && action == "ai-reply") {
            if (!store_.task(id, task)) { status = 404; statusText = "Not Found"; return jsonError("task not found"); }
            const std::wstring reply = store_.aiReply(id);
            return "{\"taskId\":" + std::to_string(id) + ",\"content\":" + (reply.empty() ? "null" : "\"" + jsonlite::escapeUtf8(reply) + "\"") + ",\"error\":" + (reply.empty() ? "\"暂无可用的答复记录\"" : "null") + "}";
        }
        if (method == "POST" && (action == "view" || action == "ignore")) {
            const bool success = store_.setStatus(id, action == "view" ? L"VIEWED" : L"IGNORED");
            if (!success) { status = 404; statusText = "Not Found"; return jsonError("task not found"); }
            store_.task(id, task);
            return "{\"success\":true,\"task\":" + taskToJson(task) + "}";
        }
        if (method == "DELETE" && action.empty()) {
            const bool success = store_.remove(id);
            if (!success) { status = 404; statusText = "Not Found"; return jsonError("task not found"); }
            return "{\"success\":true}";
        }
    }
    if (method == "DELETE" && path == "/api/tasks") {
        if (query.find("confirm") == query.end() || query.at("confirm") != "true") { status = 400; statusText = "Bad Request"; return jsonError("confirm=true is required"); }
        const std::wstring scope = queryWide(query, "scope").empty() ? L"all" : queryWide(query, "scope");
        const bool simpleScope = scope == L"all" || scope == L"completed" || scope == L"queue" || scope == L"history";
        const bool sourceScope = scope.rfind(L"source:", 0) == 0 ||
                                 scope.rfind(L"queue:source:", 0) == 0 ||
                                 scope.rfind(L"history:source:", 0) == 0;
        const size_t sourceStart = scope.rfind(L"history:source:", 0) == 0 ? 15
                                  : (scope.rfind(L"queue:source:", 0) == 0 ? 13 : 7);
        const std::wstring source = sourceScope ? scope.substr(sourceStart) : L"";
        const bool validSource = source == L"CHATGPT" || source == L"CLAUDE_CODE" ||
                                 source == L"CODEX" || source == L"OTHER";
        if ((!simpleScope && !sourceScope) || (sourceScope && !validSource)) {
            status = 400; statusText = "Bad Request"; return jsonError("unknown scope");
        }
        return "{\"success\":true,\"deleted\":" + std::to_string(store_.clear(scope)) + "}";
    }
    status = 404;
    statusText = "Not Found";
    return jsonError("not found");
}
