#ifndef CONNECTION_POOL_H
#define CONNECTION_POOL_H

#include <memory>
#include <string>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <thread>
#include <chrono>
#include <unordered_set>
#include <libpq-fe.h>

class ConnectionPool : public std::enable_shared_from_this<ConnectionPool> {
public:
	// 连接配置
	struct Config {
		std::string host = "localhost";
		std::string port = "5432";
		std::string dbname = "test";
		std::string user = "test";
		std::string password = "123456";
		int min_connections = 5;           // 最小连接数
		int max_connections = 100;          // 最大连接数
		int connection_timeout = 30;       // 连接超时（秒）
		int idle_timeout = 300;            // 空闲超时（秒）
		int max_lifetime = 3600;           // 连接最大生命周期（秒）
		bool test_on_borrow = true;        // 获取连接时测试
		bool test_on_return = false;       // 归还连接时测试
	};

	// 连接包装器
	class Connection {
	private:
		PGconn* conn_;
		std::shared_ptr<ConnectionPool> pool_;
		std::chrono::high_resolution_clock::time_point created_time_;
		std::chrono::high_resolution_clock::time_point last_used_time_;
		bool in_use_ = false;

	public:
		Connection(PGconn* conn, std::shared_ptr<ConnectionPool> pool);
		~Connection();

		// 禁止拷贝
		Connection(const Connection&) = delete;
		Connection& operator=(const Connection&) = delete;

		// 允许移动
		Connection(Connection&& other) noexcept;
		Connection& operator=(Connection&& other) noexcept;

		PGconn* raw() const { return conn_; }
		bool is_valid() const;
		bool test_connection();
		void update_last_used();
		std::chrono::seconds age() const;
		std::chrono::seconds idle_time() const;

		friend class ConnectionPool;
	};

	// 连接池状态
	struct Stats {
		int total_connections = 0;
		int active_connections = 0;
		int idle_connections = 0;
		int waiting_requests = 0;
	};

	// 获取连接池实例（单例模式）
	static std::shared_ptr<ConnectionPool> create(const Config& config);
	static std::shared_ptr<ConnectionPool> get_instance();

	~ConnectionPool();

	// 获取连接
	std::unique_ptr<Connection> get_connection();
	std::unique_ptr<Connection> get_connection(std::chrono::milliseconds timeout);

	// 连接池管理
	void shutdown();
	bool is_shutdown() const { return shutdown_.load(); }

	// 状态监控
	Stats get_stats() const;
	void print_stats() const;

	// 配置
	const Config& get_config() const { return config_; }

private:
	ConnectionPool(const Config& config);
	ConnectionPool(const ConnectionPool&) = delete;
	ConnectionPool& operator=(const ConnectionPool&) = delete;

	// 内部方法
	void init_connections();
	PGconn* create_raw_connection();
	void close_raw_connection(PGconn* conn);
	void return_connection(std::unique_ptr<Connection> conn);
	void background_task();
	void evict_idle_connections();
	void maintain_pool_size();
	bool validate_connection(PGconn* conn);

	// 连接池数据
	Config config_;
	std::queue<std::unique_ptr<Connection>> idle_connections_;
	std::unordered_set<Connection*> active_connections_;

	// 同步原语
	mutable std::mutex mutex_;
	std::condition_variable condition_;

	// 状态标志
	std::atomic<bool> shutdown_{ false };
	std::atomic<int> total_connections_{ 0 };
	std::atomic<int> waiting_requests_{ 0 };

	// 后台线程
	std::thread background_thread_;

	// 单例实例
	static std::shared_ptr<ConnectionPool> instance_;
	static std::mutex instance_mutex_;

	friend class Connection;
};

#endif // CONNECTION_POOL_H