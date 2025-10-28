#include "ConnectionPool.h"
#include <iostream>
#include <sstream>
#include <stdexcept>

// 静态成员初始化
std::shared_ptr<ConnectionPool> ConnectionPool::instance_ = nullptr;
std::mutex ConnectionPool::instance_mutex_;

// Connection 类实现
ConnectionPool::Connection::Connection(PGconn* conn, std::shared_ptr<ConnectionPool> pool)
	: conn_(conn), pool_(std::move(pool)),
	created_time_(std::chrono::high_resolution_clock::now()),
	last_used_time_(created_time_) {
}

ConnectionPool::Connection::Connection(Connection&& other) noexcept
	: conn_(other.conn_), pool_(std::move(other.pool_)),
	created_time_(other.created_time_),
	last_used_time_(other.last_used_time_),
	in_use_(other.in_use_) {
	other.conn_ = nullptr;
	other.in_use_ = false;
}

ConnectionPool::Connection& ConnectionPool::Connection::operator=(Connection&& other) noexcept {
	if (this != &other) {
		if (conn_ && pool_) {
			pool_->close_raw_connection(conn_);
		}
		conn_ = other.conn_;
		pool_ = std::move(other.pool_);
		created_time_ = other.created_time_;
		last_used_time_ = other.last_used_time_;
		in_use_ = other.in_use_;
		other.conn_ = nullptr;
		other.in_use_ = false;
	}
	return *this;
}

ConnectionPool::Connection::~Connection() {
	if (conn_ && pool_) {
		if (in_use_) {
			// 如果连接还在使用中，归还到连接池
			auto pool = pool_;
			auto wrapper = std::make_unique<Connection>(conn_, pool);
			pool_->return_connection(std::move(wrapper));
		}
		else {
			// 否则直接关闭连接
			pool_->close_raw_connection(conn_);
		}
	}
}

bool ConnectionPool::Connection::is_valid() const {
	if (!conn_) return false;
	return PQstatus(conn_) == CONNECTION_OK;
}

bool ConnectionPool::Connection::test_connection() {
	if (!is_valid()) return false;

	PGresult* res = PQexec(conn_, "SELECT 1");
	if (PQresultStatus(res) != PGRES_TUPLES_OK) {
		PQclear(res);
		return false;
	}
	PQclear(res);
	return true;
}

void ConnectionPool::Connection::update_last_used() {
	last_used_time_ = std::chrono::high_resolution_clock::now();
}

std::chrono::seconds ConnectionPool::Connection::age() const {
	return std::chrono::duration_cast<std::chrono::seconds>(
		std::chrono::high_resolution_clock::now() - created_time_);
}

std::chrono::seconds ConnectionPool::Connection::idle_time() const {
	return std::chrono::duration_cast<std::chrono::seconds>(
		std::chrono::high_resolution_clock::now() - last_used_time_);
}

// ConnectionPool 类实现
ConnectionPool::ConnectionPool(const Config& config)
	: config_(config) {

	// 验证配置
	if (config.min_connections < 1) {
		throw std::invalid_argument("min_connections must be at least 1");
	}
	if (config.max_connections < config.min_connections) {
		throw std::invalid_argument("max_connections must be >= min_connections");
	}

	// 启动后台维护线程
	background_thread_ = std::thread(&ConnectionPool::background_task, this);
}

ConnectionPool::~ConnectionPool() {
	shutdown();
}

std::shared_ptr<ConnectionPool> ConnectionPool::create(const Config& config) {
	std::lock_guard<std::mutex> lock(instance_mutex_);
	if (!instance_) {
		instance_ = std::shared_ptr<ConnectionPool>(new ConnectionPool(config));
		//instance_ = std::make_shared<ConnectionPool>(config);
		instance_->init_connections();
	}
	return instance_;
}

std::shared_ptr<ConnectionPool> ConnectionPool::get_instance() {
	std::lock_guard<std::mutex> lock(instance_mutex_);
	if (!instance_) {
		throw std::runtime_error("ConnectionPool not initialized. Call create() first.");
	}
	return instance_;
}

void ConnectionPool::init_connections()
{
	// 创建初始连接
	for (int i = 0; i < config_.min_connections; ++i) {
		auto conn = create_raw_connection();
		if (conn) {
			auto wrapper = std::make_unique<Connection>(conn, shared_from_this());
			idle_connections_.push(std::move(wrapper));
		}
	}
}

PGconn* ConnectionPool::create_raw_connection() {
	std::stringstream conninfo;
	conninfo << "host=" << config_.host
		<< " port=" << config_.port
		<< " dbname=" << config_.dbname
		<< " user=" << config_.user
		<< " password=" << config_.password
		<< " connect_timeout=" << config_.connection_timeout;

	PGconn* conn = PQconnectdb(conninfo.str().c_str());

	if (PQstatus(conn) != CONNECTION_OK) {
		std::string error = PQerrorMessage(conn);
		PQfinish(conn);
		throw std::runtime_error("Failed to create database connection: " + error);
	}

	// 设置连接参数
	PQexec(conn, "SET client_encoding TO 'UTF8'");

	total_connections_++;
	return conn;
}

void ConnectionPool::close_raw_connection(PGconn* conn) {
	if (conn) {
		PQfinish(conn);
		total_connections_--;
	}
}

bool ConnectionPool::validate_connection(PGconn* conn) {
	if (!conn || PQstatus(conn) != CONNECTION_OK) {
		return false;
	}

	if (config_.test_on_borrow) {
		PGresult* res = PQexec(conn, "SELECT 1");
		bool valid = (PQresultStatus(res) == PGRES_TUPLES_OK);
		PQclear(res);
		return valid;
	}

	return true;
}

std::unique_ptr<ConnectionPool::Connection> ConnectionPool::get_connection() {
	return get_connection(std::chrono::milliseconds(config_.connection_timeout * 1000));
}

std::unique_ptr<ConnectionPool::Connection> ConnectionPool::get_connection(
	std::chrono::milliseconds timeout) {

	if (shutdown_.load()) {
		throw std::runtime_error("Connection pool is shutdown");
	}

	std::unique_lock<std::mutex> lock(mutex_);
	waiting_requests_++;

	// 等待直到有可用连接或超时
	auto predicate = [this]() {
		return !idle_connections_.empty() ||
			total_connections_ < config_.max_connections ||
			shutdown_.load();
		};

	if (!condition_.wait_for(lock, timeout, predicate)) {
		waiting_requests_--;
		throw std::runtime_error("Timeout waiting for database connection");
	}

	waiting_requests_--;

	if (shutdown_.load()) {
		throw std::runtime_error("Connection pool is shutdown");
	}

	std::unique_ptr<Connection> conn;

	// 尝试从空闲队列获取连接
	if (!idle_connections_.empty()) {
		conn = std::move(idle_connections_.front());
		idle_connections_.pop();

		// 验证连接
		if (!validate_connection(conn->conn_)) {
			close_raw_connection(conn->conn_);
			conn->conn_ = nullptr;
			conn = nullptr;
		}
	}

	// 如果没有可用连接但还可以创建新连接
	if (!conn && total_connections_ < config_.max_connections) {
		lock.unlock();
		try {
			PGconn* raw_conn = create_raw_connection();
			conn = std::make_unique<Connection>(raw_conn, shared_from_this());
		}
		catch (const std::exception& e) {
			// 创建失败，重新获取锁并继续
			lock.lock();
			// 可能其他线程已经创建了连接，再次检查空闲队列
			if (!idle_connections_.empty()) {
				conn = std::move(idle_connections_.front());
				idle_connections_.pop();
			}
			else {
				throw;
			}
		}
		lock.lock();
	}

	if (!conn) {
		throw std::runtime_error("No available database connections");
	}

	conn->in_use_ = true;
	conn->update_last_used();
	active_connections_.insert(conn.get());

	return conn;
}

void ConnectionPool::return_connection(std::unique_ptr<Connection> conn) {
	if (!conn) return;

	std::lock_guard<std::mutex> lock(mutex_);

	// 从活跃连接中移除
	active_connections_.erase(conn.get());
	conn->in_use_ = false;

	// 检查连接是否仍然有效
	bool connection_ok = true;
	if (config_.test_on_return) {
		connection_ok = conn->test_connection();
	}
	else {
		connection_ok = conn->is_valid();
	}

	// 检查连接生命周期
	bool lifetime_ok = (conn->age() < std::chrono::seconds(config_.max_lifetime));

	if (connection_ok && lifetime_ok && !shutdown_.load()) {
		// 连接有效，放回空闲队列
		idle_connections_.push(std::move(conn));
		condition_.notify_one();
	}
	else {
		// 连接无效或超期，关闭它
		close_raw_connection(conn->conn_);
		conn->conn_ = nullptr;

		// 如果连接池没有关闭，可能需要创建新连接来维持最小连接数
		if (!shutdown_.load() && total_connections_ < config_.min_connections) {
			// 在后台任务中会处理
			condition_.notify_one();
		}
	}
}

void ConnectionPool::background_task() {
	while (!shutdown_.load()) {
		std::this_thread::sleep_for(std::chrono::seconds(10)); // 每10秒运行一次

		if (shutdown_.load()) break;

		try {
			evict_idle_connections();
			maintain_pool_size();
		}
		catch (const std::exception& e) {
			std::cerr << "ConnectionPool background task error: " << e.what() << std::endl;
		}
	}
}

void ConnectionPool::evict_idle_connections() {
	std::lock_guard<std::mutex> lock(mutex_);

	if (idle_connections_.empty()) return;

	// 计算需要驱逐的空闲连接数量（保留最小连接数）
	int connections_to_keep = std::max(config_.min_connections,
		static_cast<int>(idle_connections_.size() / 2));

	std::queue<std::unique_ptr<Connection>> new_idle_queue;
	int kept = 0;

	while (!idle_connections_.empty()) {
		auto conn = std::move(idle_connections_.front());
		idle_connections_.pop();

		bool should_keep = (kept < connections_to_keep) &&
			(conn->idle_time() < std::chrono::seconds(config_.idle_timeout)) &&
			conn->is_valid();

		if (should_keep) {
			new_idle_queue.push(std::move(conn));
			kept++;
		}
		else {
			close_raw_connection(conn->conn_);
			conn->conn_ = nullptr;
		}
	}

	idle_connections_ = std::move(new_idle_queue);
}

void ConnectionPool::maintain_pool_size() {
	std::lock_guard<std::mutex> lock(mutex_);

	int current_total = total_connections_.load();
	int current_idle = idle_connections_.size();

	// 如果总连接数小于最小连接数，创建新连接
	if (current_total < config_.min_connections) {
		int connections_to_create = config_.min_connections - current_total;

		for (int i = 0; i < connections_to_create; ++i) {
			try {
				PGconn* raw_conn = create_raw_connection();
				auto wrapper = std::make_unique<Connection>(raw_conn, shared_from_this());
				idle_connections_.push(std::move(wrapper));
			}
			catch (const std::exception& e) {
				std::cerr << "Failed to create maintenance connection: " << e.what() << std::endl;
				break;
			}
		}

		if (connections_to_create > 0) {
			condition_.notify_all();
		}
	}
}

void ConnectionPool::shutdown() {
	if (shutdown_.exchange(true)) {
		return; // 已经关闭
	}

	// 通知所有等待的线程
	condition_.notify_all();

	// 等待后台线程结束
	if (background_thread_.joinable()) {
		background_thread_.join();
	}

	// 关闭所有连接
	std::lock_guard<std::mutex> lock(mutex_);

	while (!idle_connections_.empty()) {
		auto conn = std::move(idle_connections_.front());
		idle_connections_.pop();
		close_raw_connection(conn->conn_);
		conn->conn_ = nullptr;
	}

	// 活跃连接会在析构时自动关闭
	active_connections_.clear();
}

ConnectionPool::Stats ConnectionPool::get_stats() const {
	std::lock_guard<std::mutex> lock(mutex_);

	Stats stats;
	stats.total_connections = total_connections_.load();
	stats.active_connections = active_connections_.size();
	stats.idle_connections = idle_connections_.size();
	stats.waiting_requests = waiting_requests_.load();

	return stats;
}

void ConnectionPool::print_stats() const {
	auto stats = get_stats();

	std::cout << "Connection Pool Stats:" << std::endl;
	std::cout << "  Total connections: " << stats.total_connections << std::endl;
	std::cout << "  Active connections: " << stats.active_connections << std::endl;
	std::cout << "  Idle connections: " << stats.idle_connections << std::endl;
	std::cout << "  Waiting requests: " << stats.waiting_requests << std::endl;
	std::cout << "  Pool utilization: "
		<< (stats.total_connections > 0 ?
			(100.0 * stats.active_connections / stats.total_connections) : 0)
		<< "%" << std::endl;
}