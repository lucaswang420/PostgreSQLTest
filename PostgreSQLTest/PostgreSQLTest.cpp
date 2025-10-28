// PostgreSQLTest.cpp : Defines the entry point for the application.
//

#include "PostgreSQLTest.h"
#include <iostream>
#include <libpq-fe.h>
#include <string>
#include <vector>
#include <format>
#include <utility>
#include <tuple>
#include <chrono>
#include <type_traits>
#include <functional>
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include "DatabaseManager.hpp"
#include <future>
#include "ConnectionPool.h"

class ScopeGuard {
private:
	std::function<void()> cleanup_;
	bool active_;

public:
	explicit ScopeGuard(std::function<void()> cleanup)
		: cleanup_(std::move(cleanup)), active_(true) {
	}

	~ScopeGuard() {
		if (active_) {
			cleanup_();
		}
	}

	// 禁止拷贝
	ScopeGuard(const ScopeGuard&) = delete;
	ScopeGuard& operator=(const ScopeGuard&) = delete;

	// 允许移动
	ScopeGuard(ScopeGuard&& other)
		: cleanup_(std::move(other.cleanup_)), active_(other.active_) {
		other.active_ = false;
	}

	ScopeGuard& operator=(ScopeGuard&&) = delete;

	void dismiss() { active_ = false; }
};

// 辅助函数
template<typename F>
ScopeGuard make_scope_guard(F&& f) {
	return ScopeGuard(std::forward<F>(f));
}

struct CompanyRecord {
	int id;
	std::string name;
	int age;
	std::string address;
	double salary;
};

class BatchOperation {
private:
	DatabaseManager& db;

public:
	BatchOperation(DatabaseManager& database) : db(database) {}

	// 插入方法: 使用参数化查询批量插入
	bool batchInsertWithParams(const std::vector<CompanyRecord>& records) {
		if (!db.beginTransaction()) return false;

		bool success = true;
		const char* paramValues[5];
		char idBuf[20], ageBuf[20], salaryBuf[20];

		for (const auto& record : records) {
			// 转换参数值
			snprintf(idBuf, sizeof(idBuf), "%d", record.id);
			snprintf(ageBuf, sizeof(ageBuf), "%d", record.age);
			snprintf(salaryBuf, sizeof(salaryBuf), "%.2f", record.salary);

			paramValues[0] = idBuf;
			paramValues[1] = record.name.c_str();
			paramValues[2] = ageBuf;
			paramValues[3] = record.address.c_str();
			paramValues[4] = salaryBuf;

			bool bRet = db.execParams("INSERT INTO company (id, name, age, address, salary) VALUES ($1, $2, $3, $4, $5)",
				5,				// 参数个数
				paramValues,	// 参数值数组
				0				// 结果格式：0=文本，1=二进制
			);
			if (!bRet) {
				break;
			}
		}

		if (success) {
			return db.commitTransaction();
		}
		else {
			db.rollbackTransaction();
			return false;
		}
	}

	// 插入方法: 使用COPY命令进行批量插入（最高效的方法）
	bool batchInsertWithCopy(const std::vector<CompanyRecord>& records) {
		// 开始COPY操作
		bool bRet = db.executeCopy("COPY company (id, name, age, address, salary) FROM STDIN");
		if (!bRet) {
			return false;
		}

		// 发送数据
		for (const auto& record : records) {
			std::string row = std::to_string(record.id) + "\t" +
				record.name+ "\t" +
				std::to_string(record.age) + "\t" +
				record.address + "\t" +
				std::to_string(record.salary) + "\n";

			if (!db.putCopyData(row)) {
				bRet = false;
				break;
			}
		}
		if (!bRet)
		{
			db.executeSQL("ROLLBACK");
			return false;
		}

		// 结束COPY操作
		if (!db.putCopyEnd(NULL)) {
			db.executeSQL("ROLLBACK");
			return false;
		}

		// 检查最终结果
		bool success = db.checkResult();
		if (!success) {
			std::cerr << "COPY operation failed: " << db.getLastError() << std::endl;
		}

		return success;
	}

	// 更新方法: 使用单个事务执行多个独立UPDATE
	bool batchUpdateWithTransaction(const std::vector<CompanyRecord>& records) {
		if (!db.beginTransaction()) {
			return false;
		}

		bool allSuccess = true;
		for (const auto& record : records) {
			std::string sql = "UPDATE company SET name = '" + record.name+
				"', age = " + std::to_string(record.age) + 
				", address = '" + record.address +
				"', salary = " + std::to_string(record.salary) +
				" WHERE id = " + std::to_string(record.id);

			if (!db.executeSQL(sql)) {
				allSuccess = false;
				break;
			}
		}

		if (allSuccess) {
			return db.commitTransaction();
		}
		else {
			db.rollbackTransaction();
			return false;
		}
	}

	// 更新方法: 使用参数化查询批量插入
	bool batchUpdateWithParams(const std::vector<CompanyRecord>& records) {
		if (!db.beginTransaction()) return false;

		bool success = true;
		const char* paramValues[5];
		char idBuf[20], ageBuf[20], salaryBuf[20];

		for (const auto& record : records) {
			// 转换参数值
			snprintf(idBuf, sizeof(idBuf), "%d", record.id);
			snprintf(ageBuf, sizeof(ageBuf), "%d", record.age);
			snprintf(salaryBuf, sizeof(salaryBuf), "%.2f", record.salary);

			paramValues[0] = idBuf;
			paramValues[1] = record.name.c_str();
			paramValues[2] = ageBuf;
			paramValues[3] = record.address.c_str();
			paramValues[4] = salaryBuf;

			const char* updateSql = "UPDATE company SET name = $2, age = $3, address = $4, salary = $5 WHERE id = $1";
			bool bRet = db.execParams(updateSql,
				5,				// 参数个数
				paramValues,	// 参数值数组
				0				// 结果格式：0=文本，1=二进制
			);
			if (!bRet) {
				success = false;
				break;
			}
		}

		if (success) {
			return db.commitTransaction();
		}
		else {
			db.rollbackTransaction();
			return false;
		}
	}

	// 更新方法: 使用参数化查询批量插入
	bool batchUpdateWithPreparedStatement(const std::vector<CompanyRecord>& records) {
		
		// 生成唯一的预处理语句名称
		std::string stmtName = "update_company_" + std::to_string(time(NULL));

		// 准备预处理语句
		const char* prepareSql = "PREPARE update_company (int, text, int, CHAR(50), NUMERIC) AS "
			"UPDATE company SET name = $2, age = $3, address = $4, salary = $5 WHERE id = $1";
		if (!db.executeSQL(prepareSql)) {
			std::cerr << "Prepare statement failed: " << db.getLastError() << std::endl;
			return false;
		}

		auto guard = make_scope_guard([this]() {
			// 清理预处理语句
			std::string cleanupSql = "DEALLOCATE update_company";
			db.executeSQL(cleanupSql.c_str());
			});

		if (!db.beginTransaction()) return false;

		bool success = true;

		const char* paramValues[5];
		char idBuf[20], ageBuf[20], salaryBuf[20];

		for (const auto& record : records) {
			// 转换参数值
			snprintf(idBuf, sizeof(idBuf), "%d", record.id);
			snprintf(ageBuf, sizeof(ageBuf), "%d", record.age);
			snprintf(salaryBuf, sizeof(salaryBuf), "%.2f", record.salary);

			paramValues[0] = idBuf;
			paramValues[1] = record.name.c_str();
			paramValues[2] = ageBuf;
			paramValues[3] = record.address.c_str();
			paramValues[4] = salaryBuf;

			const char* updateSql = "UPDATE company SET name = $2, age = $3, address = $4, salary = $5 WHERE id = $1";
			bool bRet = db.execPrepared("update_company",
				5,				// 参数个数
				paramValues,	// 参数值数组
				0				// 结果格式：0=文本，1=二进制
			);
			if (!bRet) {
				success = false;
				break;
			}
		}

		if (success) {
			return db.commitTransaction();
		}
		else {
			db.rollbackTransaction();
			return false;
		}
	}

	// 更新方法: 使用CASE语句进行批量更新（更高效）
	bool batchUpdateWithCASE(const std::vector<CompanyRecord>& records) {
		if (records.empty()) return true;

		const size_t BATCH_SIZE = 1000; // 每批处理1000条
		bool overallSuccess = true;

		for (size_t start = 0; start < records.size(); start += BATCH_SIZE) {
			size_t end = std::min(start + BATCH_SIZE, records.size());
			std::vector<CompanyRecord> batch(records.begin() + start, records.begin() + end);

			if (!batchUpdateWithCASEImpl(batch)) {
				overallSuccess = false;
				// 可以选择继续处理或中断
				break;
			}

			// 可选：输出进度
			if (start % 5000 == 0) {
				std::cout << "Processed " << start << " records..." << std::endl;
			}
		}

		return overallSuccess;
	}

	// 更新方法: 使用临时表+COPY进行批量更新（大数据量）
	bool batchUpdateWithTempTable(const std::vector<CompanyRecord>& records) {
		if (records.empty()) return true;

		std::string tempTable = "temp_updates_" + std::to_string(rand());

		// 开始事务
		if (!db.beginTransaction()) return false;

		bool success = false;

		try {
			// 1. 创建临时表
			std::string createTemp = "CREATE TEMPORARY TABLE " + tempTable +
				" (id INT PRIMARY KEY, name TEXT, age INT, address CHAR(50), salary NUMERIC) ON COMMIT DROP";
			if (!db.executeSQL(createTemp)) {
				throw std::runtime_error("Failed to create temp table");
			}

			// 2. 使用 COPY 快速插入数据到临时表
			// 启动 COPY
			std::string copyCmd = "COPY " + tempTable + " (id, name, age, address, salary) FROM STDIN";
			if (!db.executeCopy(copyCmd)) {
				return false;
			}
			// 分批发送COPY数据，避免内存压力
			constexpr int COPY_BATCH_SIZE = 10000;
			for (size_t i = 0; i < records.size(); i += COPY_BATCH_SIZE) {
				size_t end = std::min(i + COPY_BATCH_SIZE, records.size());

				std::string batchData;
				for (size_t j = i; j < end; ++j) {
					const auto& record = records[j];
					batchData += std::to_string(record.id) + "\t" +
						record.name+ "\t" +
						std::to_string(record.age) + "\t" +
						record.address + "\t" +
						std::to_string(record.salary) + "\n";
				}

				if (!db.putCopyData(batchData)) {
					return false;
				}
			}
			// 结束 COPY
			if (!db.putCopyEnd(NULL)) {
				return false;
			}

			// 3. 使用 JOIN 更新目标表
			std::string updateSql =
				"UPDATE company SET "
				"name = t.name, age = t.age, address = t.address, salary = t.salary "
				"FROM " + tempTable + " t "
				"WHERE company.id = t.id";

			if (!db.executeSQL(updateSql)) {
				throw std::runtime_error("Failed to update from temp table");
			}

			success = true;

		}
		catch (const std::exception& e) {
			std::cerr << "Temp table batch update failed: " << e.what() << std::endl;
			success = false;
		}

		// 提交或回滚事务
		if (success) {
			return db.commitTransaction();
		}
		else {
			db.rollbackTransaction();
			return false;
		}
	}

	// 更新方法: 并行批量更新（复杂实现，未完成）
	bool parallelBatchUpdate(const std::vector<CompanyRecord>& records, int threadcount = 4)
	{
		size_t totalRecords = records.size();
		size_t recordsPerThread = (totalRecords + threadcount - 1) / threadcount;
		std::vector<std::future<bool>> futures;
		for (int i = 0; i < threadcount; ++i) {
			size_t startIdx = i * recordsPerThread;
			size_t endIdx = std::min(startIdx + recordsPerThread, totalRecords);
			if (startIdx >= endIdx) break;
			std::vector<CompanyRecord> threadRecords(records.begin() + startIdx, records.begin() + endIdx);
			futures.push_back(std::async(std::launch::async, [this, threadRecords]() {
				DatabaseManager threadDb;
				BatchOperation batchOp(threadDb);
				return batchOp.batchUpdateWithTempTable(threadRecords);
				}));
		}
		bool allSuccess = true;
		for (auto& fut : futures) {
			if (!fut.get()) {
				allSuccess = false;
			}
		}
		return allSuccess;
	}

	private:
		bool batchUpdateWithCASEImpl(const std::vector<CompanyRecord>& records) {
			if (records.empty()) return true;

			// 构建 CASE 语句
			std::string sql = "UPDATE company SET "
				"name = CASE id ";
			for (const auto& record : records) {
				sql += "WHEN " + std::to_string(record.id) + " THEN '" + record.name + "' ";
			}
			sql += "ELSE name END, ";

			sql += "age = CASE id ";
			for (const auto& record : records) {
				sql += "WHEN " + std::to_string(record.id) + " THEN " + std::to_string(record.age) + " ";
			}
			sql += "ELSE age END, ";

			sql += "address = CASE id ";
			for (const auto& record : records) {
				sql += "WHEN " + std::to_string(record.id) + " THEN '" + record.address + "' ";
			}
			sql += "ELSE address END, ";

			sql += "salary = CASE id ";
			for (const auto& record : records) {
				sql += "WHEN " + std::to_string(record.id) + " THEN " + std::to_string(record.salary) + " ";
			}
			sql += "ELSE salary END ";

			// WHERE 子句
			sql += "WHERE id IN (";
			for (size_t i = 0; i < records.size(); ++i) {
				if (i > 0) sql += ", ";
				sql += std::to_string(records[i].id);
			}
			sql += ")";

			return db.executeSQL(sql);
		}
};

int BasicTest();
int BatchOperationTest(int size);
int DropTableIfExists();

int main() {
	// 设置控制台输出代码页为UTF-8
	::SetConsoleOutputCP(CP_UTF8);

	ConnectionPool::Config config;
	ConnectionPool::create(config);

	DropTableIfExists();
	int nRet = BasicTest();
	BatchOperationTest(100);
	BatchOperationTest(500);
	BatchOperationTest(1000);
	BatchOperationTest(5000);
	BatchOperationTest(10000);
	BatchOperationTest(50000);
	BatchOperationTest(100000);
	DropTableIfExists();
	
	return 0;
}

int BatchOperationTest(int size)
{
	//小数据量 (< 100)：使用 PQexecParams 逐条更新，简单可靠
	//中等数据量(100 - 1000)：使用 CASE 语句 或 预处理语句
	//大数据量(> 1000)：使用 临时表 + COPY 方法
	//需要复杂业务逻辑：使用 PQexecParams 逐条更新，便于错误处理
	//性能要求极高：使用 临时表 + COPY

	std::cout << "\n---------------Batch Update Test(" << size << " records)--------------------\n";

	try {
		// 连接数据库
		DatabaseManager db;

		// 创建测试表
		db.executeSQL("DROP TABLE IF EXISTS company");
		db.executeSQL("CREATE TABLE company ("
			"id INT PRIMARY KEY, "
			"name TEXT NOT NULL, "
			"age INT NOT NULL, "
			"address CHAR(50), "
			"salary REAL)");

		BatchOperation batchExample(db);
		{
			// 示例: 参数化批量插入
			std::cout << "Method 1 (Parameterized batch insert): ";

			std::vector<CompanyRecord> records;
			for (int i = 1; i <= size; ++i)
			{
				records.push_back({ i, "Name" + std::to_string(i), 20 + (i % 30), "Address" + std::to_string(i), 30000.0 + (i * 10) });
			}
			auto start = std::chrono::high_resolution_clock::now();
			if (batchExample.batchInsertWithParams(records)) {
				std::cout << "Success" << std::endl;
			}
			else {
				std::cout << "Failed" << std::endl;
			}
			auto end = std::chrono::high_resolution_clock::now();
			auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

			std::cout << "Parameterized insert method cost " << duration_ms << " ms" << std::endl;
		}
		{
			// 示例: COPY批量插入
			std::cout << "Method 2 (COPY batch insert): ";

			std::vector<CompanyRecord> copyRecords;
			for (int i = size + 1; i <= 2 * size; ++i)
			{
				copyRecords.push_back({ i, "Name" + std::to_string(i), 20 + (i % 30), "Address" + std::to_string(i), 30000.0 + (i * 10) });
			}
			auto start = std::chrono::high_resolution_clock::now();
			if (batchExample.batchInsertWithCopy(copyRecords)) {
				std::cout << "Success" << std::endl;
			}
			else {
				std::cout << "Failed" << std::endl;
			}
			auto end = std::chrono::high_resolution_clock::now();
			auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

			std::cout << "COPY insert method cost " << duration_ms << " ms" << std::endl;
		}
		{
			// 示例: 基本事务批量更新
			std::cout << "Method 3 (batch UPDATEs in transaction): ";

			std::vector<CompanyRecord> records;
			for (int i = 1; i <= size; ++i)
			{
				records.push_back({ i, "TransactionName" + std::to_string(i), 100 + (i % 30), "TransactionAddress" + std::to_string(i), 10000.0 + (i * 10) });
			}
			std::chrono::high_resolution_clock::time_point start = std::chrono::high_resolution_clock::now();
			if (batchExample.batchUpdateWithTransaction(records)) {
				std::cout << "Success" << std::endl;
			}
			else {
				std::cout << "Failed" << std::endl;
			}
			std::chrono::high_resolution_clock::time_point end = std::chrono::high_resolution_clock::now();
			auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

			std::cout << "transaction UPDATE method cost " << duration_ms << " ms" << std::endl;
		}
		{
			// 示例: 参数化批量更新
			std::cout << "Method 4 (Parameterized batch UPDATE): ";

			std::vector<CompanyRecord> records;
			for (int i = 1; i <= size; ++i)
			{
				records.push_back({ i, "ParameterizedName" + std::to_string(i), 200 + (i % 30), "ParameterizedAddress" + std::to_string(i), 20000.0 + (i * 10) });
			}
			auto start = std::chrono::high_resolution_clock::now();
			if (batchExample.batchUpdateWithParams(records)) {
				std::cout << "Success" << std::endl;
			}
			else {
				std::cout << "Failed" << std::endl;
			}
			auto end = std::chrono::high_resolution_clock::now();
			auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

			std::cout << "Parameterized UPDATE method cost " << duration_ms << " ms" << std::endl;
		}
		{
			// 示例: 预处理语句批量更新
			std::cout << "Method 5 (PreparedStatement batch UPDATE): ";

			std::vector<CompanyRecord> records;
			for (int i = 1; i <= size; ++i)
			{
				records.push_back({ i, "PreparedName" + std::to_string(i), 600 + (i % 30), "PreparedAddress" + std::to_string(i), 60000.0 + (i * 10) });
			}
			auto start = std::chrono::high_resolution_clock::now();
			if (batchExample.batchUpdateWithPreparedStatement(records)) {
				std::cout << "Success" << std::endl;
			}
			else {
				std::cout << "Failed" << std::endl;
			}
			auto end = std::chrono::high_resolution_clock::now();
			auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

			std::cout << "PreparedStatement UPDATE method cost " << duration_ms << " ms" << std::endl;
		}
		if(size <= 10000)
		{
			// 示例: 单个CASE批量更新
			std::cout << "Method 6 (CASE batch UPDATE): ";

			std::vector<CompanyRecord> records;
			for (int i = 1; i <= size; ++i)
			{
				records.push_back({ i, "CASEName" + std::to_string(i), 300 + (i % 30), "CASEAddress" + std::to_string(i), 30000.0 + (i * 10) });
			}
			auto start = std::chrono::high_resolution_clock::now();
			if (batchExample.batchUpdateWithCASE(records)) {
				std::cout << "Success" << std::endl;
			}
			else {
				std::cout << "Failed" << std::endl;
			}
			auto end = std::chrono::high_resolution_clock::now();
			auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

			std::cout << "CASE method cost " << duration_ms << " ms" << std::endl;
		}
		{
			// 示例: 临时表批量更新
			std::cout << "Method 7 (TempTable+COPY batch UPDATE): ";

			std::vector<CompanyRecord> records;
			for (int i = 1; i <= size; ++i)
			{
				records.push_back({ i, "TempTableName" + std::to_string(i), 400 + (i % 30), "TempTableAddress" + std::to_string(i), 40000.0 + (i * 10) });
			}
			auto start = std::chrono::high_resolution_clock::now();
			if (batchExample.batchUpdateWithTempTable(records)) {
				std::cout << "Success" << std::endl;
			}
			else {
				std::cout << "Failed" << std::endl;
			}
			auto end = std::chrono::high_resolution_clock::now();
			auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

			std::cout << "TempTable UPDATE method cost " << duration_ms << " ms" << std::endl;
		}
		if (size >= 1000)
		{
			// 示例: 并行批量更新
			std::cout << "Method 8 (Parallel COPY batch UPDATE): ";

			std::vector<CompanyRecord> records;
			for (int i = 1; i <= size; ++i)
			{
				records.push_back({ i, "ParallelName" + std::to_string(i), 400 + (i % 30), "ParallelAddress" + std::to_string(i), 40000.0 + (i * 10) });
			}
			auto start = std::chrono::high_resolution_clock::now();
			if (batchExample.parallelBatchUpdate(records, ConnectionPool::get_instance()->get_stats().total_connections)) {
				std::cout << "Success" << std::endl;
			}
			else {
				std::cout << "Failed" << std::endl;
			}
			auto end = std::chrono::high_resolution_clock::now();
			auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

			std::cout << "Parallel UPDATE method cost " << duration_ms << " ms" << std::endl;
		}
	}

	catch (const std::exception& e) {
		std::cerr << "Error: " << e.what() << std::endl;
		return 1;
	}
	return 0;
}

int DropTableIfExists()
{
	DatabaseManager db;
	return db.executeSQL("DROP TABLE IF EXISTS company");
}	

int BasicTest()
{
	std::cout << "\n---------------PQ basic testing--------------------\n";

	// 1. 连接数据库
	PGconn* conn = PQconnectdb("dbname=postgres user=postgres password=159357 hostaddr=127.0.0.1 port=5432");

	if (PQstatus(conn) != CONNECTION_OK) {
		std::cerr << "Connection to database failed: " << PQerrorMessage(conn);
		PQfinish(conn);
		return 1;
	}
	std::cout << "Opened database successfully" << std::endl;

	// 2. 创建表
	const char* createTableSQL =
		"CREATE TABLE IF NOT EXISTS COMPANY("
		"ID INT PRIMARY KEY     NOT NULL,"
		"NAME           TEXT    NOT NULL,"
		"AGE            INT     NOT NULL,"
		"ADDRESS        CHAR(50),"
		"SALARY         REAL);";

	PGresult* res = PQexec(conn, createTableSQL);
	if (PQresultStatus(res) != PGRES_COMMAND_OK) {
		std::cerr << "CREATE TABLE failed: " << PQerrorMessage(conn);
		PQclear(res);
		PQfinish(conn);
		return 1;
	}
	PQclear(res);
	std::cout << "Table created successfully" << std::endl;

	// 3. 插入数据
	const char* insertSQL =
		"INSERT INTO COMPANY (ID,NAME,AGE,ADDRESS,SALARY) "
		"VALUES (1, 'Paul', 32, 'California', 25000.00 );";

	res = PQexec(conn, insertSQL);
	if (PQresultStatus(res) != PGRES_COMMAND_OK) {
		std::cerr << "INSERT failed: " << PQerrorMessage(conn);
		PQclear(res);
		PQfinish(conn);
		return 1;
	}
	PQclear(res);
	std::cout << "Record inserted successfully" << std::endl;

	// 4. 查询数据
	const char* querySQL = "SELECT * from COMPANY";
	res = PQexec(conn, querySQL);

	if (PQresultStatus(res) != PGRES_TUPLES_OK) {
		std::cerr << "SELECT failed: " << PQerrorMessage(conn);
		PQclear(res);
		PQfinish(conn);
		return 1;
	}

	int nTuples = PQntuples(res);
	int nFields = PQnfields(res);
	std::cout << "Number of records returned: " << nTuples << std::endl;

	// 打印列名
	for (int i = 0; i < nFields; i++) {
		printf("%-15s", PQfname(res, i));
	}
	printf("\n\n");

	// 打印数据行
	for (int i = 0; i < nTuples; i++) {
		for (int j = 0; j < nFields; j++) {
			printf("%-15s", PQgetvalue(res, i, j));
		}
		printf("\n");
	}
	PQclear(res);

	// 5. 更新数据
	const char* updateSQL = "UPDATE COMPANY SET SALARY = 30000 WHERE ID = 1";
	res = PQexec(conn, updateSQL);
	if (PQresultStatus(res) != PGRES_COMMAND_OK) {
		std::cerr << "UPDATE failed: " << PQerrorMessage(conn);
		PQclear(res);
		PQfinish(conn);
		return 1;
	}
	PQclear(res);
	std::cout << "Record updated successfully" << std::endl;

	{
		// 查询数据
		const char* querySQL = "SELECT * from COMPANY";
		res = PQexec(conn, querySQL);

		if (PQresultStatus(res) != PGRES_TUPLES_OK) {
			std::cerr << "SELECT failed: " << PQerrorMessage(conn);
			PQclear(res);
			PQfinish(conn);
			return 1;
		}

		int nTuples = PQntuples(res);
		std::cout << "Number of records returned: " << nTuples << std::endl;

		// 打印数据行
		for (int i = 0; i < nTuples; i++) {
			for (int j = 0; j < nFields; j++) {
				printf("%-15s", PQgetvalue(res, i, j));
			}
			printf("\n");
		}
		PQclear(res);
	}

	// 6. 删除数据
	const char* deleteSQL = "DELETE FROM COMPANY WHERE ID = 1";
	res = PQexec(conn, deleteSQL);
	if (PQresultStatus(res) != PGRES_COMMAND_OK) {
		std::cerr << "DELETE failed: " << PQerrorMessage(conn);
		PQclear(res);
		PQfinish(conn);
		return 1;
	}
	PQclear(res);
	std::cout << "Record deleted successfully" << std::endl;

	{
		// 查询数据
		const char* querySQL = "SELECT * from COMPANY";
		res = PQexec(conn, querySQL);

		if (PQresultStatus(res) != PGRES_TUPLES_OK) {
			std::cerr << "SELECT failed: " << PQerrorMessage(conn);
			PQclear(res);
			PQfinish(conn);
			return 1;
		}

		int nTuples = PQntuples(res);
		std::cout << "Number of records returned: " << nTuples << std::endl;

		// 打印数据行
		for (int i = 0; i < nTuples; i++) {
			for (int j = 0; j < nFields; j++) {
				printf("%-15s", PQgetvalue(res, i, j));
			}
			printf("\n");
		}
		PQclear(res);
	}

	//DROP TABLE [IF EXISTS] table_name [CASCADE | RESTRICT];
	//CASCADE：自动删除依赖于该表的对象（如视图、外键约束等）
	//RESTRICT：如果有任何对象依赖于该表，则拒绝删除（默认选项）
	const char* deleteTableSQL = "DROP TABLE IF EXISTS COMPANY RESTRICT;";
	res = PQexec(conn, deleteTableSQL);
	if (PQresultStatus(res) != PGRES_COMMAND_OK) {
		std::cerr << "DELETE TABLE failed: " << PQerrorMessage(conn);
		PQclear(res);
		PQfinish(conn);
		return 1;
	}
	PQclear(res);
	std::cout << "Table deleted successfully" << std::endl;

	// 关闭连接
	PQfinish(conn);

	return 0;
}