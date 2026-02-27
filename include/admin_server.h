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
#include <functional>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

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
     */
    using OnQuery = std::function<nlohmann::json(const std::string &queryType)>;

    AdminServer(uint16_t port = 9900);
    ~AdminServer();

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

    // 已连接的客户端 socket fd 列表
    std::vector<int> clientFds_;
    std::mutex clientsMutex_;

    // 回调
    OnOrder onOrder_;
    OnCancel onCancel_;
    OnQuery onQuery_;

    int serverFd_ = -1;

    /**
     * @brief 接受连接的主循环
     */
    void acceptLoop();

    /**
     * @brief 处理单个客户端连接
     */
    void handleClient(int clientFd);

    /**
     * @brief 向指定 fd 发送 JSON 消息（自动追加 '\n'）
     * @return true 发送成功，false 连接已断开
     */
    bool sendToFd(int fd, const nlohmann::json &message);
};

} // namespace hdf
