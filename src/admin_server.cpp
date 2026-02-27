/**
 * @file admin_server.cpp
 * @brief 管理界面 TCP 服务端实现
 *
 * 功能：
 * 1. acceptLoop() — 创建 TCP 服务端，接受客户端连接
 * 2. handleClient() — 读取 JSON Lines，根据 type 分发给回调
 * 3. broadcast() — 向所有已连接客户端推送回报
 * 4. sendToFd() — 线程安全地发送数据
 *
 * 协议：JSON Lines（每条 JSON 对象 + '\n'）
 */

#include "admin_server.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <iostream>
#include <string>

namespace hdf {

AdminServer::AdminServer(uint16_t port) : port_(port) {}

AdminServer::~AdminServer() { stop(); }

void AdminServer::setOnOrder(OnOrder callback) { onOrder_ = callback; }

void AdminServer::setOnCancel(OnCancel callback) { onCancel_ = callback; }

void AdminServer::setOnQuery(OnQuery callback) { onQuery_ = callback; }

void AdminServer::start() {
    if (running_)
        return;
    running_ = true;
    acceptThread_ = std::thread(&AdminServer::acceptLoop, this);
}

void AdminServer::stop() {
    running_ = false;

    // 关闭 server socket，使 accept() 立即返回错误从而退出循环
    if (serverFd_ >= 0) {
        ::shutdown(serverFd_, SHUT_RDWR);
        ::close(serverFd_);
        serverFd_ = -1;
    }

    if (acceptThread_.joinable()) {
        acceptThread_.join();
    }

    // 关闭所有客户端连接
    std::lock_guard<std::mutex> lock(clientsMutex_);
    for (int fd : clientFds_) {
        ::shutdown(fd, SHUT_RDWR);
        ::close(fd);
    }
    clientFds_.clear();
}

void AdminServer::broadcast(const nlohmann::json &message) {
    // 包装为 response 类型
    nlohmann::json wrapped = message;
    wrapped["type"] = "response";

    std::lock_guard<std::mutex> lock(clientsMutex_);

    // 发送并移除已断开的连接
    auto it = clientFds_.begin();
    while (it != clientFds_.end()) {
        if (!sendToFd(*it, wrapped)) {
            ::close(*it);
            it = clientFds_.erase(it);
        } else {
            ++it;
        }
    }
}

void AdminServer::acceptLoop() {
    // 1. 创建 socket
    serverFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (serverFd_ < 0) {
        std::cerr << "[AdminServer] socket() failed: " << std::strerror(errno)
                  << std::endl;
        running_ = false;
        return;
    }

    // 2. SO_REUSEADDR — 允许快速重启
    int opt = 1;
    ::setsockopt(serverFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // 3. bind
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    // 管理端口默认仅监听本机回环地址 127.0.0.1
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port_);

    if (::bind(serverFd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) <
        0) {
        std::cerr << "[AdminServer] bind() port " << port_
                  << " failed: " << std::strerror(errno) << std::endl;
        ::close(serverFd_);
        serverFd_ = -1;
        running_ = false;
        return;
    }

    // 4. listen
    if (::listen(serverFd_, 8) < 0) {
        std::cerr << "[AdminServer] listen() failed: " << std::strerror(errno)
                  << std::endl;
        ::close(serverFd_);
        serverFd_ = -1;
        running_ = false;
        return;
    }

    std::cout << "[AdminServer] Listening on port " << port_ << std::endl;

    // 5. accept loop
    while (running_) {
        sockaddr_in clientAddr{};
        socklen_t clientLen = sizeof(clientAddr);
        int clientFd = ::accept(
            serverFd_, reinterpret_cast<sockaddr *>(&clientAddr), &clientLen);

        if (clientFd < 0) {
            if (!running_)
                break; // stop() 被调用
            std::cerr << "[AdminServer] accept() failed: "
                      << std::strerror(errno) << std::endl;
            continue;
        }

        // TCP_NODELAY — 减少小包延迟
        int flag = 1;
        ::setsockopt(clientFd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

        char addrStr[INET_ADDRSTRLEN];
        ::inet_ntop(AF_INET, &clientAddr.sin_addr, addrStr, sizeof(addrStr));
        std::cout << "[AdminServer] Client connected from " << addrStr << ":"
                  << ntohs(clientAddr.sin_port) << " (fd=" << clientFd << ")"
                  << std::endl;

        {
            std::lock_guard<std::mutex> lock(clientsMutex_);
            clientFds_.push_back(clientFd);
        }

        // 每个客户端一个线程处理
        std::thread(&AdminServer::handleClient, this, clientFd).detach();
    }
}

void AdminServer::handleClient(int clientFd) {
    // 按 '\n' 分隔读取 JSON Lines
    std::string buffer;
    char chunk[4096];

    while (running_) {
        ssize_t n = ::recv(clientFd, chunk, sizeof(chunk), 0);
        if (n <= 0) {
            // n == 0: 对端关闭; n < 0: 错误
            break;
        }

        buffer.append(chunk, static_cast<size_t>(n));

        // 处理所有完整的行（以 '\n' 分隔）
        size_t pos;
        while ((pos = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, pos);
            buffer.erase(0, pos + 1);

            if (line.empty())
                continue;

            try {
                auto msg = nlohmann::json::parse(line);
                std::string type = msg.value("type", "");

                if (type == "order") {
                    // 提取订单字段，去掉 type 后转发给 TradeSystem
                    nlohmann::json orderJson;
                    orderJson["clOrderId"] = msg.value("clOrderId", "");
                    orderJson["market"] = msg.value("market", "");
                    orderJson["securityId"] = msg.value("securityId", "");
                    orderJson["side"] = msg.value("side", "");
                    orderJson["price"] = msg.value("price", 0.0);
                    orderJson["qty"] = msg.value("qty", 0);
                    orderJson["shareholderId"] = msg.value("shareholderId", "");
                    // 透传 target 字段（gateway / exchange）
                    if (msg.contains("target")) {
                        orderJson["target"] = msg["target"];
                    }

                    if (onOrder_) {
                        onOrder_(orderJson);
                    }
                } else if (type == "cancel") {
                    // 提取撤单字段
                    nlohmann::json cancelJson;
                    cancelJson["clOrderId"] = msg.value("clOrderId", "");
                    cancelJson["origClOrderId"] =
                        msg.value("origClOrderId", "");
                    cancelJson["market"] = msg.value("market", "");
                    cancelJson["securityId"] = msg.value("securityId", "");
                    cancelJson["shareholderId"] =
                        msg.value("shareholderId", "");
                    cancelJson["side"] = msg.value("side", "");
                    // 透传 target 字段
                    if (msg.contains("target")) {
                        cancelJson["target"] = msg["target"];
                    }

                    if (onCancel_) {
                        onCancel_(cancelJson);
                    }
                } else if (type == "query") {
                    std::string queryType = msg.value("queryType", "orderbook");
                    if (onQuery_) {
                        nlohmann::json result = onQuery_(queryType);
                        result["type"] = "snapshot";
                        sendToFd(clientFd, result);
                    }
                } else {
                    // 未知消息类型
                    nlohmann::json err;
                    err["type"] = "error";
                    err["message"] = "Unknown message type: " + type;
                    sendToFd(clientFd, err);
                }
            } catch (const nlohmann::json::parse_error &e) {
                nlohmann::json err;
                err["type"] = "error";
                err["message"] = std::string("JSON parse error: ") + e.what();
                sendToFd(clientFd, err);
            } catch (const std::exception &e) {
                nlohmann::json err;
                err["type"] = "error";
                err["message"] = std::string("Internal error: ") + e.what();
                sendToFd(clientFd, err);
            }
        }
    }

    // 客户端断开 — 从列表中移除
    std::cout << "[AdminServer] Client disconnected (fd=" << clientFd << ")"
              << std::endl;
    {
        std::lock_guard<std::mutex> lock(clientsMutex_);
        clientFds_.erase(
            std::remove(clientFds_.begin(), clientFds_.end(), clientFd),
            clientFds_.end());
    }
    ::close(clientFd);
}

bool AdminServer::sendToFd(int fd, const nlohmann::json &message) {
    std::string data = message.dump() + "\n";
    ssize_t totalSent = 0;
    ssize_t remaining = static_cast<ssize_t>(data.size());

    while (remaining > 0) {
        ssize_t sent = ::send(fd, data.c_str() + totalSent,
                              static_cast<size_t>(remaining), MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EINTR)
                continue; // 被信号中断，重试
            // EPIPE / ECONNRESET 等 → 连接已断开
            return false;
        }
        totalSent += sent;
        remaining -= sent;
    }
    return true;
}

} // namespace hdf
