#pragma once

/**
 * @brief 管理界面 TCP 服务端
 *
 * 提供 TCP 接口供 Python 管理界面连接，支持：
 * - 接收订单/撤单指令 → 转发给 TradeSystem
 * - 接收查询请求 → 返回订单簿快照
 * - 广播回报 → 推送给所有已连接的管理客户端
 *
 * 协议：JSON Lines（每条消息一个 JSON 对象 + '\n'）
 */

#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <unordered_map>

namespace hdf {

class AdminServer {
  public:
    /**
     * @brief 收到管理界面发来的订单时的回调
     * 应该调用 TradeSystem::handleOrder
     */
    using OnOrder = std::function<void(const nlohmann::json &)>;

    /**
     * @brief 收到管理界面发来的撤单时的回调
     * 应该调用 TradeSystem::handleCancel
     */
    using OnCancel = std::function<void(const nlohmann::json &)>;

    /**
     * @brief 收到查询请求时的回调
     * 应该返回订单簿快照等信息
     * msg 包含 queryType, 以及可选的 securityId / market 筛选字段
     */
    using OnQuery = std::function<nlohmann::json(const nlohmann::json &queryMsg)>;

    AdminServer(uint16_t port = 32000);
    ~AdminServer();

    /**
     * @brief 设置是否打印日志（默认 true）
     */
    void setVerbose(bool v) { verbose_ = v; }

    // 设置回调
    void setOnOrder(OnOrder callback);
    void setOnCancel(OnCancel callback);
    void setOnQuery(OnQuery callback);

    /**
     * @brief 启动服务（在新线程中监听）
     */
    void start();

    /**
     * @brief 停止服务
     */
    void stop();

    /**
     * @brief 向所有已连接的管理客户端广播回报
     * 在 TradeSystem 的 sendToClient_ 回调中调用
     */
    void broadcast(const nlohmann::json &message);

  private:
    uint16_t port_;
    std::atomic<bool> running_{false};
    std::thread acceptThread_;

    struct PendingMessage {
        std::string data;
        size_t offset = 0;
    };

    struct ClientInfo {
        int fd;
        std::string readBuffer;
        std::deque<PendingMessage> writeQueue;
        bool wantWrite = false;
    };

    // 已连接客户端
    std::unordered_map<int, ClientInfo> clients_;
    std::mutex clientsMutex_;

    // 回调
    OnOrder onOrder_;
    OnCancel onCancel_;
    OnQuery onQuery_;

    bool verbose_ = true;
    int serverFd_ = -1;
    int epollFd_ = -1;

    /**
     * @brief 接受连接的主循环
     */
    void acceptLoop();

    void acceptNewClients();
    void handleReadable(int clientFd);
    void handleWritable(int clientFd);
    void processLine(int clientFd, const std::string &line);
    void closeClient(int clientFd);
    void updateEpollEventsLocked(int fd, ClientInfo &client, bool wantWrite);
    bool enqueueMessageLocked(int fd, ClientInfo &client,
                              std::string &&payload);
    bool sendToFd(int fd, const nlohmann::json &message);
    static bool makeSocketNonBlocking(int fd);
};

} // namespace hdf
