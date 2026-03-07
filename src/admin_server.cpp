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
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <iostream>
#include <string>
#include <vector>

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
        ::shutdown(serverFd_, SHUT_RDWR);
    }

    if (acceptThread_.joinable()) {
        acceptThread_.join();
    }

    // 确保所有残留的客户端连接和 epoll 描述符都被关闭
    std::lock_guard<std::mutex> lock(clientsMutex_);
    for (auto &[fd, client] : clients_) {
        ::shutdown(fd, SHUT_RDWR);
        ::close(fd);
    }
    clients_.clear();

    if (serverFd_ >= 0) {
        ::close(serverFd_);
        serverFd_ = -1;
    }
    if (epollFd_ >= 0) {
        ::close(epollFd_);
        epollFd_ = -1;
    }
}

void AdminServer::broadcast(const nlohmann::json &message) {
    std::string payload = message.dump() + "\n";
    std::lock_guard<std::mutex> lock(clientsMutex_);
    for (auto &[fd, client] : clients_) {
        enqueueMessageLocked(fd, client, std::string(payload));
    }
}

// 经典的多路复用实现
void AdminServer::acceptLoop() {
    serverFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (serverFd_ < 0) {
        if (verbose_)
            std::cerr << "[AdminServer] socket() failed: "
                      << std::strerror(errno) << std::endl;
        running_ = false;
        return;
    }

    makeSocketNonBlocking(serverFd_);

    int opt = 1;
    ::setsockopt(serverFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port_);

    if (::bind(serverFd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) <
        0) {
        if (verbose_)
            std::cerr << "[AdminServer] bind() port " << port_
                      << " failed: " << std::strerror(errno) << std::endl;
        ::close(serverFd_);
        serverFd_ = -1;
        running_ = false;
        return;
    }

    if (::listen(serverFd_, 128) < 0) {
        if (verbose_)
            std::cerr << "[AdminServer] listen() failed: "
                      << std::strerror(errno) << std::endl;
        ::close(serverFd_);
        serverFd_ = -1;
        running_ = false;
        return;
    }

    epollFd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (epollFd_ < 0) {
        if (verbose_)
            std::cerr << "[AdminServer] epoll_create1() failed: "
                      << std::strerror(errno) << std::endl;
        ::close(serverFd_);
        serverFd_ = -1;
        running_ = false;
        return;
    }

    epoll_event listenEvent{};
    listenEvent.data.fd = serverFd_;
    listenEvent.events = EPOLLIN;
    ::epoll_ctl(epollFd_, EPOLL_CTL_ADD, serverFd_, &listenEvent);

    if (verbose_)
        std::cout << "[AdminServer] Listening on port " << port_ << std::endl;

    std::vector<epoll_event> events(128);

    while (running_) {
        int ready = ::epoll_wait(epollFd_, events.data(),
                                 static_cast<int>(events.size()), 1000);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            if (verbose_)
                std::cerr << "[AdminServer] epoll_wait() failed: "
                          << std::strerror(errno) << std::endl;
            break;
        }

        for (int i = 0; i < ready; ++i) {
            int fd = events[i].data.fd;
            uint32_t ev = events[i].events;

            if (fd == serverFd_) {
                acceptNewClients();
                continue;
            }

            if (ev & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
                closeClient(fd);
                continue;
            }

            if (ev & EPOLLIN)
                handleReadable(fd);
            if (ev & EPOLLOUT)
                handleWritable(fd);
        }
    }

    {
        std::lock_guard<std::mutex> lock(clientsMutex_);
        for (auto &[fd, client] : clients_) {
            ::shutdown(fd, SHUT_RDWR);
            ::close(fd);
        }
        clients_.clear();
    }

    if (epollFd_ >= 0) {
        ::close(epollFd_);
        epollFd_ = -1;
    }
    if (serverFd_ >= 0) {
        ::close(serverFd_);
        serverFd_ = -1;
    }
}

void AdminServer::acceptNewClients() {
    while (true) {
        sockaddr_in clientAddr{};
        socklen_t clientLen = sizeof(clientAddr);
        int clientFd =
            ::accept4(serverFd_, reinterpret_cast<sockaddr *>(&clientAddr),
                      &clientLen, SOCK_NONBLOCK);
        if (clientFd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            if (errno == EINTR)
                continue;
            if (verbose_)
                std::cerr << "[AdminServer] accept4() failed: "
                          << std::strerror(errno) << std::endl;
            break;
        }

        int flag = 1;
        ::setsockopt(clientFd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

        epoll_event ev{};
        ev.data.fd = clientFd;
        ev.events = EPOLLIN | EPOLLRDHUP;
        ::epoll_ctl(epollFd_, EPOLL_CTL_ADD, clientFd, &ev);

        {
            std::lock_guard<std::mutex> lock(clientsMutex_);
            clients_.emplace(clientFd, ClientInfo{clientFd});
        }

        if (verbose_) {
            char addrStr[INET_ADDRSTRLEN];
            ::inet_ntop(AF_INET, &clientAddr.sin_addr, addrStr,
                        sizeof(addrStr));
            std::cout << "[AdminServer] Client connected from " << addrStr
                      << ":" << ntohs(clientAddr.sin_port)
                      << " (fd=" << clientFd << ")" << std::endl;
        }
    }
}

void AdminServer::handleReadable(int clientFd) {
    char buffer[4096];
    while (true) {
        ssize_t n = ::recv(clientFd, buffer, sizeof(buffer), 0);
        if (n > 0) {
            std::vector<std::string> lines;
            {
                std::lock_guard<std::mutex> lock(clientsMutex_);
                auto it = clients_.find(clientFd);
                if (it == clients_.end())
                    return;
                it->second.readBuffer.append(buffer, static_cast<size_t>(n));
                size_t pos;
                while ((pos = it->second.readBuffer.find('\n')) !=
                       std::string::npos) {
                    std::string line = it->second.readBuffer.substr(0, pos);
                    it->second.readBuffer.erase(0, pos + 1);
                    if (!line.empty())
                        lines.push_back(std::move(line));
                }
            }

            for (auto &line : lines) {
                processLine(clientFd, line);
            }
            continue;
        }

        if (n == 0) {
            closeClient(clientFd);
            return;
        }

        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }

        closeClient(clientFd);
        return;
    }
}

void AdminServer::handleWritable(int clientFd) {
    while (true) {
        std::string chunk;
        {
            std::lock_guard<std::mutex> lock(clientsMutex_);
            auto it = clients_.find(clientFd);
            if (it == clients_.end())
                return;
            ClientInfo &client = it->second;
            if (client.writeQueue.empty()) {
                if (client.wantWrite) {
                    updateEpollEventsLocked(clientFd, client, false);
                }
                return;
            }
            PendingMessage &msg = client.writeQueue.front();
            chunk.assign(msg.data.c_str() + msg.offset,
                         msg.data.size() - msg.offset);
        }

        ssize_t sent =
            ::send(clientFd, chunk.data(), chunk.size(), MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            closeClient(clientFd);
            return;
        }

        bool morePending = false;
        {
            std::lock_guard<std::mutex> lock(clientsMutex_);
            auto it = clients_.find(clientFd);
            if (it == clients_.end())
                return;
            ClientInfo &client = it->second;
            if (client.writeQueue.empty())
                return;
            PendingMessage &msg = client.writeQueue.front();
            msg.offset += static_cast<size_t>(sent);
            if (msg.offset >= msg.data.size()) {
                client.writeQueue.pop_front();
            }
            morePending = !client.writeQueue.empty();
            if (!morePending) {
                updateEpollEventsLocked(clientFd, client, false);
            }
        }

        if (!morePending)
            return;
    }
}

void AdminServer::processLine(int clientFd, const std::string &line) {
    try {
        auto msg = nlohmann::json::parse(line);
        std::string type = msg.value("type", "");

        if (type == "order") {
            nlohmann::json orderJson;
            orderJson["clOrderId"] = msg.value("clOrderId", "");
            orderJson["market"] = msg.value("market", "");
            orderJson["securityId"] = msg.value("securityId", "");
            orderJson["side"] = msg.value("side", "");
            orderJson["price"] = msg.value("price", 0.0);
            orderJson["qty"] = msg.value("qty", 0);
            orderJson["shareholderId"] = msg.value("shareholderId", "");
            if (msg.contains("target")) {
                orderJson["target"] = msg["target"];
            }
            if (onOrder_)
                onOrder_(orderJson);
        } else if (type == "cancel") {
            nlohmann::json cancelJson;
            cancelJson["clOrderId"] = msg.value("clOrderId", "");
            cancelJson["origClOrderId"] = msg.value("origClOrderId", "");
            cancelJson["market"] = msg.value("market", "");
            cancelJson["securityId"] = msg.value("securityId", "");
            cancelJson["shareholderId"] = msg.value("shareholderId", "");
            cancelJson["side"] = msg.value("side", "");
            if (msg.contains("target")) {
                cancelJson["target"] = msg["target"];
            }
            if (onCancel_)
                onCancel_(cancelJson);
        } else if (type == "query") {
            if (onQuery_) {
                nlohmann::json result = onQuery_(msg);
                result["type"] = "snapshot";
                sendToFd(clientFd, result);
            }
        } else {
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

void AdminServer::closeClient(int clientFd) {
    std::lock_guard<std::mutex> lock(clientsMutex_);
    auto it = clients_.find(clientFd);
    if (it == clients_.end())
        return;

    if (epollFd_ >= 0)
        ::epoll_ctl(epollFd_, EPOLL_CTL_DEL, clientFd, nullptr);
    ::shutdown(clientFd, SHUT_RDWR);
    ::close(clientFd);
    clients_.erase(it);

    if (verbose_)
        std::cout << "[AdminServer] Client disconnected (fd=" << clientFd << ")"
                  << std::endl;
}

void AdminServer::updateEpollEventsLocked(int fd, ClientInfo &client,
                                          bool wantWrite) {
    if (epollFd_ < 0)
        return;
    uint32_t events = EPOLLIN | EPOLLRDHUP;
    if (wantWrite)
        events |= EPOLLOUT;
    epoll_event ev{};
    ev.data.fd = fd;
    ev.events = events;
    ::epoll_ctl(epollFd_, EPOLL_CTL_MOD, fd, &ev);
    client.wantWrite = wantWrite;
}

bool AdminServer::enqueueMessageLocked(int fd, ClientInfo &client,
                                       std::string &&payload) {
    client.writeQueue.push_back(PendingMessage{std::move(payload), 0});
    if (!client.wantWrite) {
        updateEpollEventsLocked(fd, client, true);
    }
    return true;
}

bool AdminServer::sendToFd(int fd, const nlohmann::json &message) {
    std::string payload = message.dump() + "\n";
    std::lock_guard<std::mutex> lock(clientsMutex_);
    auto it = clients_.find(fd);
    if (it == clients_.end())
        return false;
    return enqueueMessageLocked(fd, it->second, std::move(payload));
}

bool AdminServer::makeSocketNonBlocking(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags == -1)
        return false;
    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
        return false;
    return true;
}

} // namespace hdf
