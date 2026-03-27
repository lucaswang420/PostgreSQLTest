# PostgreSQL C 语言开发测试项目

这是一个用于学习和测试 PostgreSQL 数据库 C 语言开发的示例项目，演示了如何在 Windows 平台上使用 `libpq` 进行数据库操作。

## 目录

- [项目简介](#项目简介)
- [组件说明](#组件说明)
- [安装步骤](#安装步骤)
- [开发环境配置](#开发环境配置)
- [快速开始](#快速开始)
- [常见问题](#常见问题)

---

## 项目简介

本项目包含了一个完整的 PostgreSQL 数据库连接示例，演示了以下功能：

- 数据库连接与认证
- 基本 SQL 查询执行
- 结果集处理
- 事务管理
- 异常处理

**开发环境**：Windows + Visual Studio + PostgreSQL libpq
**编程语言**：C

---

## 组件说明

| 组件 | 主要作用 | 获取与说明 |
| :--- | :--- | :--- |
| **PostgreSQL 服务器** | 提供数据库服务核心，包含运行实例所需的所有程序 | 从官网下载安装包，通常会自动安装 |
| **C 客户端库 (libpq)** | C 程序连接和操作 PostgreSQL 数据库的主要库，包含头文件和链接库 | 在安装服务器时，**务必在组件选择步骤勾选 "PostgreSQL C Libraries (libpq)"** |

---

## 安装步骤

### 1. 下载安装包

访问 [PostgreSQL 官网下载页面](https://www.postgresql.org/download/windows/)，选择适合你 Windows 系统的安装程序（通常是 64 位）。建议选择较新的稳定版本。

### 2. 运行安装程序

运行下载的安装程序（例如 `postgresql-16-X-windows-x64.exe`）。

在 **"Select Components"**（选择组件）步骤中，确保：

- **`PostgreSQL Server`** 必须被选中（默认选项）
- **`Command Line Tools`** 被选中（安装 `psql` 等命令行工具）
- **`PostgreSQL C Libraries (libpq)`** 被选中（**关键步骤**）

后续步骤按提示操作：

- 设置超级用户（`postgres`）密码
- 选择端口（默认 5432）

---

## 开发环境配置

安装成功后，PostgreSQL 通常位于 `C:\Program Files\PostgreSQL\16` 目录。

### 1. 包含头文件

在 C 源代码中包含 libpq 头文件：

```c
#include <libpq-fe.h>
```

在项目中配置：

- **Visual Studio**：项目属性 → **C/C++** → **常规** → **附加包含目录**
- 添加路径：`C:\Program Files\PostgreSQL\16\include`

### 2. 链接库文件

配置链接器：

- **库目录**：添加 `C:\Program Files\PostgreSQL\16\lib`
- **附加依赖项**：添加 `libpq.lib`

### 3. 运行时依赖

编译后的可执行文件运行时需要 `libpq.dll`，该文件位于 `C:\Program Files\PostgreSQL\16\bin`。

确保程序能找到该 DLL：

- 将 `bin` 目录添加到系统 `PATH` 环境变量
- 或将 `libpq.dll` 复制到可执行文件所在目录

---

## 快速开始

### 创建测试数据库

使用 `psql` 命令行工具或 pgAdmin 执行：

```sql
CREATE DATABASE test_db;
\c test_db
CREATE TABLE users (
    id SERIAL PRIMARY KEY,
    name VARCHAR(100),
    email VARCHAR(100)
);
```

### 示例代码

```c
#include <stdio.h>
#include <libpq-fe.h>

int main() {
    // 连接数据库
    PGconn *conn = PQconnectdb("host=localhost port=5432 dbname=test_db user=postgres password=your_password");

    // 检查连接状态
    if (PQstatus(conn) != CONNECTION_OK) {
        fprintf(stderr, "连接失败: %s", PQerrorMessage(conn));
        PQfinish(conn);
        return 1;
    }

    printf("成功连接到 PostgreSQL!\n");

    // 执行查询
    PGresult *res = PQexec(conn, "SELECT version()");
    if (PQresultStatus(res) == PGRES_TUPLES_OK) {
        printf("PostgreSQL 版本: %s\n", PQgetvalue(res, 0, 0));
    }
    PQclear(res);

    // 关闭连接
    PQfinish(conn);
    return 0;
}
```

### 编译命令

```bash
gcc -I"C:\Program Files\PostgreSQL\16\include" \
    -L"C:\Program Files\PostgreSQL\16\lib" \
    -o test_app test.c -lpq
```

---

## 常见问题

### 连接失败

- 确认 PostgreSQL 服务已启动（在服务管理器中查看 `postgresql-x64-16`）
- 检查连接参数是否正确
- 验证防火墙未阻止端口 5432

### 编译错误

- 确保头文件路径和库文件路径配置正确
- 检查架构一致性（32 位/64 位）
- Visual Studio 中确保在正确配置（Debug/Release）下设置

### 运行时错误

- 确认 `libpq.dll` 在可访问的位置
- 检查数据库权限设置
- 验证数据库名称和用户权限

### 远程连接配置

如需远程连接，修改配置文件：

1. `postgresql.conf`：设置 `listen_addresses = '*'`
2. `pg_hba.conf`：添加访问规则

修改后重启 PostgreSQL 服务。

---

## 许可证

本项目仅供学习和参考使用。
