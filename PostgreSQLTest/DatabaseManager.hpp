#pragma once

#include <iostream>
#include <string>
#include <libpq-fe.h>
#include "ConnectionPool.h"

class DatabaseManager {
private:
	std::unique_ptr<ConnectionPool::Connection> m_conn = nullptr;
	std::string m_lastError;

public:
	DatabaseManager() {
		std::shared_ptr<ConnectionPool> spConnectPool = ConnectionPool::get_instance();
		m_conn = spConnectPool->get_connection();
		if (!m_conn->test_connection()){
			throw std::runtime_error(PQerrorMessage(m_conn->raw()));
		}
	}

	~DatabaseManager() {
		if (m_conn) m_conn.reset(nullptr);
	}

	// 开始事务
	bool beginTransaction() {
		PGresult* res = PQexec(m_conn->raw(), "BEGIN");
		bool success = (PQresultStatus(res) == PGRES_COMMAND_OK);
		if (!success)
		{
			m_lastError = PQerrorMessage(m_conn->raw());
		}
		PQclear(res);
		return success;
	}

	// 提交事务
	bool commitTransaction() {
		PGresult* res = PQexec(m_conn->raw(), "COMMIT");
		bool success = (PQresultStatus(res) == PGRES_COMMAND_OK);
		if (!success)
		{
			m_lastError = PQerrorMessage(m_conn->raw());
		}
		PQclear(res);
		return success;
	}

	// 回滚事务
	bool rollbackTransaction() {
		PGresult* res = PQexec(m_conn->raw(), "ROLLBACK");
		bool success = (PQresultStatus(res) == PGRES_COMMAND_OK);
		if (!success)
		{
			m_lastError = PQerrorMessage(m_conn->raw());
		}
		PQclear(res);
		return success;
	}

	// 执行单条SQL
	bool executeSQL(const std::string& sql) {
		PGresult* res = PQexec(m_conn->raw(), sql.c_str());
		bool success = (PQresultStatus(res) == PGRES_COMMAND_OK);
		if (!success) {
			m_lastError = PQerrorMessage(m_conn->raw());
			std::cerr << "SQL Error: " << m_lastError << std::endl;
		}
		PQclear(res);
		return success;
	}

	bool executeCopy(const std::string& sql) {
		PGresult* res = PQexec(m_conn->raw(), sql.c_str());
		bool success = (PQresultStatus(res) == PGRES_COPY_IN);
		if (!success) {
			m_lastError = PQerrorMessage(m_conn->raw());
			std::cerr << "COPY Error: " << m_lastError << std::endl;
		}
		PQclear(res);
		return success;
	}

	bool putCopyData(const std::string& buffer)
	{
		if (PQputCopyData(m_conn->raw(), buffer.c_str(), (int)buffer.length()) != 1) {
			m_lastError = PQerrorMessage(m_conn->raw());
			std::cerr << "PQputCopyData Error: " << m_lastError << std::endl;
			return false;
		}
		return true;
	}

	bool putCopyEnd(const char* errormsg)
	{
		if (PQputCopyEnd(m_conn->raw(), errormsg) != 1)
		{
			m_lastError = PQerrorMessage(m_conn->raw());
			std::cerr << "PQputCopyEnd Error: " << m_lastError << std::endl;
			return false;
		}
		return true;
	}

	bool checkResult()
	{
		PGresult* res = PQgetResult(m_conn->raw());
		bool success = (PQresultStatus(res) == PGRES_COMMAND_OK);
		PQclear(res);
		return success;
	}

	bool execParams(const std::string& sql, int nParams, const char* const* paramValues, int resultFormat)
	{
		PGresult* res = PQexecParams(m_conn->raw(),
			sql.c_str(),
			nParams,       // 参数个数
			NULL,    // 参数类型OID数组，NULL让PostgreSQL推断类型
			paramValues, // 参数值数组
			NULL,    // 参数长度数组
			NULL,    // 参数格式数组
			resultFormat        // 结果格式：0=文本，1=二进制
		);

		bool success = (PQresultStatus(res) == PGRES_COMMAND_OK);
		if (!success) {
			m_lastError = PQerrorMessage(m_conn->raw());
			std::cerr << "SQL Error: " << m_lastError << std::endl;
		}
		PQclear(res);
		return success;
	}

	bool execPrepared(const std::string& statementName, int nParams, const char* const* paramValues, int resultFormat)
	{
		PGresult* res = PQexecPrepared(m_conn->raw(),
			statementName.c_str(),
			nParams,       // 参数个数
			paramValues, // 参数值数组
			NULL,    // 参数长度数组
			NULL,    // 参数格式数组
			resultFormat        // 结果格式：0=文本，1=二进制
		);

		bool success = (PQresultStatus(res) == PGRES_COMMAND_OK);
		if (!success) {
			m_lastError = PQerrorMessage(m_conn->raw());
			std::cerr << "SQL Error: " << m_lastError << std::endl;
		}
		PQclear(res);
		return success;
	}

	std::string getLastError() const {
		return m_lastError;
	}
};