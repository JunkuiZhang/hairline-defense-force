/**
 * @file admin_server.cpp
 * @brief 管理界面 TCP 服务端实现
 *
 * TODO: 由组员实现以下功能：
 * 1. acceptLoop() — socket 创建、bind、listen、accept
 * 2. handleClient() — 读取 JSON Lines，根据 type 分发给回调
 * 3. broadcast() — 向所有客户端推送回报
 * 4. sendToFd() — 线程安全地发送数据
 *
 * 提示：
 * - 使用 POSIX socket API（sys/socket.h, netinet/in.h）
 * - 每个客户端连接用一个 std::thread 处理
 * - JSON 解析使用 nlohmann::json::parse
 * - 消息以 '\n' 分隔（JSON Lines 格式）
 */

#include "admin_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <sstream>
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
    if (serverFd_ >= 0) {
        ::close(serverFd_);
        serverFd_ = -1;
    }
    if (acceptThread_.joinable()) {
        acceptThread_.join();
    }
    // 关闭所有客户端连接
    std::lock_guard<std::mutex> lock(clientsMutex_);
    for (int fd : clientFds_) {
        ::close(fd);
    }
    clientFds_.clear();
}

void AdminServer::broadcast(const nlohmann::json &message) {
    std::lock_guard<std::mutex> lock(clientsMutex_);
    // 移除已断开的连接，向存活连接发送
    for (int fd : clientFds_) {
        sendToFd(fd, message);
    }
}

void AdminServer::acceptLoop() {
    // TODO: 实现 TCP accept 循环
    //
    // 伪代码：
    // 1. serverFd_ = socket(AF_INET, SOCK_STREAM, 0)
    // 2. setsockopt(SO_REUSEADDR)
    // 3. bind(port_)
    // 4. listen()
    // 5. while (running_):
    //      clientFd = accept(...)
    //      clientFds_.push_back(clientFd)
    //      std::thread(&AdminServer::handleClient, this, clientFd).detach()
}

void AdminServer::handleClient(int clientFd) {
    // TODO: 实现客户端消息处理循环
    //
    // 伪代码：
    // 1. 从 clientFd 读取数据（按 '\n' 分隔）
    // 2. 对每条完整消息：
    //      json msg = json::parse(line)
    //      string type = msg["type"]
    //      if type == "order":
    //          从 msg 中提取订单字段，构造订单 JSON
    //          onOrder_(orderJson)
    //      elif type == "cancel":
    //          从 msg 中提取撤单字段，构造撤单 JSON
    //          onCancel_(cancelJson)
    //      elif type == "query":
    //          json result = onQuery_(msg["queryType"])
    //          sendToFd(clientFd, result)
    // 3. 连接断开时从 clientFds_ 中移除
}

void AdminServer::sendToFd(int fd, const nlohmann::json &message) {
    std::string data = message.dump() + "\n";
    // TODO: 完善错误处理（EPIPE 等）
    ::send(fd, data.c_str(), data.size(), MSG_NOSIGNAL);
}

} // namespace hdf
