/**
 * @file test_session_hostgroup_metrics-t.cpp
 * @brief Test for checking the metrics from stats table 'stats_mysql_hostgroups_latencies'.
 * @details The following checks are performed:
 *   - Test 1: Open multiple connections to ProxySQL, perform one query per connection and check that:
 *       1. The average waiting time match the expected when a waiting is imposed via 'max_connections=0'.
 *       2. 'sessions_waiting' and 'sessions_waited' match the number of oppened connections when the
 *          connections are still oppened.
 *       3. Once limitation is removed, connections are returned and queries executed, and 'sessions_waiting'
 *          pass to have '0' value and 'conns_total' and 'queries_total' match the number of oppened
 *   - Test 2. Open a transaction and check that:
 *       1. The number of connections != number of queries executed in the hostgroup.
 *       2. Hostgroup tracking is properly performed, transaction queries being counted in 'hostgroup 0' and
 *          SELECTS in 'hostgroup 1'.
 *   - Test 3. Checks that the number of waiting sessions is decreased accordingly when a sessions times out
 *     without getting a connection, and the waited time is updated properly.
 *   - Test 4: Imposes a connection limit for a server, open multiple connections to ProxySQL, perform one
 *     query per connection against that server and checks that:
 *       1. The connections exceeding the limit are represented in 'sessions_waiting' metric.
 *       2. Once all the connections are served, all the metrics are properly updated.
 */

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <stdio.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <sys/resource.h>

#include <mysql.h>
#include <mysql/mysqld_error.h>

#include "command_line.h"
#include "json.hpp"
#include "tap.h"
#include "utils.h"

using std::vector;
using std::string;
using std::map;

const uint32_t MAX_NUM_CONNECTIONS = 10000;

int main(int argc, char** argv) {
	CommandLine cl;

	if (cl.getEnv()) {
		diag("Failed to get the required environmental variables.");
		return EXIT_FAILURE;
	}

	// Just in case more than 1024 connections want to be tried
	struct rlimit limits { 0, 0 };
	getrlimit(RLIMIT_NOFILE, &limits);
	diag("test_session_hostgroup_metrics-t: Old process limits: { %ld, %ld }", limits.rlim_cur, limits.rlim_max);
	limits.rlim_cur = MAX_NUM_CONNECTIONS;
	setrlimit(RLIMIT_NOFILE, &limits);
	diag("test_session_hostgroup_metrics-t: New process limits: { %ld, %ld }", limits.rlim_cur, limits.rlim_max);

	MYSQL* proxysql_admin = mysql_init(NULL);

	if (!mysql_real_connect(proxysql_admin, cl.host, cl.admin_username, cl.admin_password, NULL, cl.admin_port, NULL, 0)) {
		fprintf(stderr, "File %s, line %d, Error: %s\n", __FILE__, __LINE__, mysql_error(proxysql_admin));
		return EXIT_FAILURE;
	}

	MYSQL_QUERY(proxysql_admin, "LOAD MYSQL SERVERS FROM DISK");
	MYSQL_QUERY(proxysql_admin, "LOAD MYSQL SERVERS TO RUNTIME");
	MYSQL_QUERY(proxysql_admin, "SELECT * FROM stats_mysql_hostgroups_latencies_reset");
	mysql_free_result(mysql_store_result(proxysql_admin));
	usleep(500 * 1000);

	// TEST 1.
	{
		const uint32_t CONN_NUM = 10;
		double EPSILON = 0.1;
		uint32_t WAITED_TIME = 3;
		const uint32_t COLUMNS_NUM = 6;

		vector<MYSQL*> proxy_conns {};
		int conn_err = open_connections(cl, CONN_NUM, proxy_conns);
		if (conn_err) { return EXIT_FAILURE; }

		MYSQL_QUERY(proxysql_admin, "UPDATE mysql_servers SET max_connections=0");
		MYSQL_QUERY(proxysql_admin, "LOAD MYSQL SERVERS TO RUNTIME");

		vector<std::thread> query_threads {};
		std::mutex query_res_mutex;
		vector<int> query_err_codes {};

		std::transform(
			proxy_conns.begin(), proxy_conns.end(), std::back_inserter(query_threads),
			[&query_res_mutex, &query_err_codes](MYSQL* conn) -> std::thread {
				return std::thread([&query_res_mutex, &query_err_codes,conn]() -> void {
					mysql_query(conn, "SELECT 1");
					std::lock_guard<std::mutex> lock_guard(query_res_mutex);
					query_err_codes.push_back(mysql_errno(conn));
				});
			}
		);

		sleep(WAITED_TIME);

		MYSQL_QUERY(proxysql_admin, "SELECT * FROM stats_mysql_hostgroups_latencies WHERE hostgroup=1");
		MYSQL_RES* latencies_res = mysql_store_result(proxysql_admin);
		map<string, vector<string>> row_map = fetch_row_values(latencies_res);
		mysql_free_result(latencies_res);

		ok(
			row_map.size() == COLUMNS_NUM && row_map["hostgroup"].size() == 1,
			"Numbers of columns should match expected and there should be only one row per hostgroup"
		);

		MYSQL_QUERY(proxysql_admin, "UPDATE mysql_servers SET max_connections=10");
		MYSQL_QUERY(proxysql_admin, "LOAD MYSQL SERVERS TO RUNTIME");

		std::for_each(query_threads.begin(), query_threads.end(), [](std::thread& th) -> void { th.join(); });
		std::for_each(proxy_conns.begin(), proxy_conns.end(), [](MYSQL* conn) -> void { mysql_close(conn); });

		// Check that all the queries succeeded because sessions received the connection from connection pool
		bool conns_succeed = std::accumulate(
			query_err_codes.begin(), query_err_codes.end(), true,
			[](bool res, int res_code) { return res == true && res_code == 0; }
		);

		uint32_t sessions_waited = std::stoll(row_map["sessions_waited"][0], NULL, 10);
		uint32_t sessions_waited_time_total = std::stoll(row_map["sessions_waited_time_total"][0], NULL, 10);
		double avg_waiting_time = sessions_waited == 0 ? 0 :
			(sessions_waited_time_total / static_cast<double>(sessions_waited)) / pow(10,6);

		ok(
			conns_succeed && avg_waiting_time > WAITED_TIME - EPSILON && avg_waiting_time < WAITED_TIME + EPSILON,
			"Connections succeed and average waiting time should match explicit waited time:"
			" { conn_succeed: '%d', exp: '%d', act: '%f', epsilon: '%f' }",
			conns_succeed, WAITED_TIME, avg_waiting_time, EPSILON
		);

		uint32_t sessions_waiting = std::stoll(row_map["sessions_waiting"][0], NULL, 10);
		uint32_t conns_total = std::stoll(row_map["conns_total"][0], NULL, 10);
		uint32_t queries_total = std::stoll(row_map["queries_total"][0], NULL, 10);

		ok(
			sessions_waiting == CONN_NUM && sessions_waited == CONN_NUM && conns_total == 0 && queries_total == 0,
			"'sessions_waiting' should be equal to CONN_NUM and 'conns_total' and 'queries_total' should be zero:"
			" { sessions_waiting: '%d', conns_total': '%d', queries_total: '%d', CONN_NUM: '%d' }",
			sessions_waiting, conns_total, queries_total, CONN_NUM
		);

		MYSQL_QUERY(proxysql_admin, "SELECT * FROM stats_mysql_hostgroups_latencies WHERE hostgroup=1");
		latencies_res = mysql_store_result(proxysql_admin);
		row_map = fetch_row_values(latencies_res);
		mysql_free_result(latencies_res);

		sessions_waited = std::stoll(row_map["sessions_waited"][0], NULL, 10);
		sessions_waiting = std::stoll(row_map["sessions_waiting"][0], NULL, 10);
		conns_total = std::stoll(row_map["conns_total"][0], NULL, 10);
		queries_total = std::stoll(row_map["queries_total"][0], NULL, 10);

		ok(
			sessions_waiting == 0 && sessions_waited == CONN_NUM && conns_total == CONN_NUM && queries_total == CONN_NUM,
			"'sessions_waiting' should be equal to '0' and 'sessions_waited', 'conns_total' and 'queries_total' should be CONN_NUM:"
			" { sessions_waiting: '%d', sessions_waited: '%d', conns_total': '%d', queries_total: '%d', CONN_NUM: '%d' }",
			sessions_waiting, sessions_waited, conns_total, queries_total, CONN_NUM
		);
	}

	MYSQL_QUERY(proxysql_admin, "LOAD MYSQL SERVERS FROM DISK");
	MYSQL_QUERY(proxysql_admin, "LOAD MYSQL SERVERS TO RUNTIME");
	MYSQL_QUERY(proxysql_admin, "SELECT * FROM stats_mysql_hostgroups_latencies_reset");
	mysql_free_result(mysql_store_result(proxysql_admin));
	usleep(500 * 1000);

	// TEST 2.
	{
		uint32_t HG0_QUERY_NUM = 0;
		uint32_t HG0_DO_1_QUERIES = 100;
		uint32_t WAITED_TIME = 1;

		MYSQL* proxysql_mysql = mysql_init(NULL);

		if (!mysql_real_connect(proxysql_mysql, cl.host, cl.username, cl.password, NULL, cl.port, NULL, 0)) {
			fprintf(stderr, "File %s, line %d, Error: %s\n", __FILE__, __LINE__, mysql_error(proxysql_mysql));
			return EXIT_FAILURE;
		}

		MYSQL_QUERY(proxysql_mysql, "BEGIN");
		HG0_QUERY_NUM += 1;

		for (uint32_t i = 0; i < HG0_DO_1_QUERIES; i++) {
			mysql_query(proxysql_mysql, "DO 1");
			HG0_QUERY_NUM += 1;
		}

		MYSQL_QUERY(proxysql_mysql, "COMMIT");
		HG0_QUERY_NUM += 1;

		MYSQL_QUERY(proxysql_admin, "SELECT * FROM stats_mysql_hostgroups_latencies WHERE hostgroup=0");
		MYSQL_RES* latencies_res = mysql_store_result(proxysql_admin);
		map<string, vector<string>> row_map = fetch_row_values(latencies_res);
		mysql_free_result(latencies_res);

		uint32_t queries_total = std::stoll(row_map["queries_total"][0], NULL, 10);
		uint32_t conns_total = std::stoll(row_map["conns_total"][0], NULL, 10);

		ok(
			queries_total == HG0_QUERY_NUM && conns_total == 1,
			"'queries_total' should match 'HG0_QUERY_NUM' but 'conns_total' should be '1':"
			" { queries_total: '%d', conns_total: '%d' }", queries_total, conns_total
		);

		MYSQL_QUERY(proxysql_mysql, "BEGIN");
		HG0_QUERY_NUM += 1;

		MYSQL_QUERY(proxysql_admin, "UPDATE mysql_servers SET max_connections=0");
		MYSQL_QUERY(proxysql_admin, "LOAD MYSQL SERVERS TO RUNTIME");

		MYSQL_QUERY(proxysql_mysql, "DO 1");
		HG0_QUERY_NUM += 1;

		sleep(WAITED_TIME);

		MYSQL_QUERY(proxysql_admin, "SELECT * FROM stats_mysql_hostgroups_latencies WHERE hostgroup=0");
		latencies_res = mysql_store_result(proxysql_admin);
		row_map = fetch_row_values(latencies_res);
		mysql_free_result(latencies_res);

		uint32_t sessions_waited = std::stoll(row_map["sessions_waited"][0], NULL, 10);
		uint32_t sessions_waited_time_total = std::stoll(row_map["sessions_waited_time_total"][0], NULL, 10);
		uint32_t sessions_waiting = std::stoll(row_map["sessions_waiting"][0], NULL, 10);

		conns_total = std::stoll(row_map["conns_total"][0], NULL, 10);
		queries_total = std::stoll(row_map["queries_total"][0], NULL, 10);

		ok(
			sessions_waiting == 0 && sessions_waited == 0 && sessions_waited_time_total == 0,
			"No waiting took place since the session already got the connection when 'max_connections' value was changed:"
			" { sessions_waiting: '%d', sessions_waited: '%d', sessions_waited_time_total: '%d' }",
			sessions_waiting, sessions_waited, sessions_waited_time_total
		);

		ok(
			conns_total == 2 && queries_total == HG0_QUERY_NUM,
			"'conns_total' should be '2' and query total should have been increased accordingly"
		);

		mysql_close(proxysql_mysql);
	}


	MYSQL_QUERY(proxysql_admin, "LOAD MYSQL SERVERS FROM DISK");
	MYSQL_QUERY(proxysql_admin, "LOAD MYSQL SERVERS TO RUNTIME");
	MYSQL_QUERY(proxysql_admin, "SELECT * FROM stats_mysql_hostgroups_latencies_reset");
	mysql_free_result(mysql_store_result(proxysql_admin));
	usleep(500 * 1000);

	// TEST 3.
	{
		const uint32_t CONN_NUM = 10;
		const uint32_t CONNECT_TIMEOUT_SERVER_MAX = 3000;
		double EPSILON = 0.5;

		MYSQL_QUERY(
			proxysql_admin,
			string {"SET mysql-connect_timeout_server_max=" + std::to_string(CONNECT_TIMEOUT_SERVER_MAX)}.c_str()
		);
		MYSQL_QUERY(proxysql_admin, "LOAD MYSQL VARIABLES TO RUNTIME");

		vector<MYSQL*> proxy_conns {};
		int conn_err = open_connections(cl, CONN_NUM, proxy_conns);
		if (conn_err) { return EXIT_FAILURE; }

		MYSQL_QUERY(proxysql_admin, "UPDATE mysql_servers SET max_connections=0");
		MYSQL_QUERY(proxysql_admin, "LOAD MYSQL SERVERS TO RUNTIME"); usleep(500 * 1000);

		vector<std::thread> query_threads {};

		std::mutex query_res_mutex;
		vector<int> query_err_codes {};

		std::transform(
			proxy_conns.begin(), proxy_conns.end(), std::back_inserter(query_threads),
			[&query_res_mutex, &query_err_codes](MYSQL* conn) -> std::thread {
				return std::thread([&query_res_mutex, &query_err_codes,conn]() -> void {
					mysql_query(conn, "SELECT 1");
					std::lock_guard<std::mutex> lock_guard(query_res_mutex);
					query_err_codes.push_back(mysql_errno(conn));
				});
			}
		);

		usleep(CONNECT_TIMEOUT_SERVER_MAX * 1000 + 100*1000);

		std::for_each(query_threads.begin(), query_threads.end(), [](std::thread& th) -> void { th.join(); });
		std::for_each(proxy_conns.begin(), proxy_conns.end(), [](MYSQL* conn) -> void { mysql_close(conn); });

		// Wait until ProxySQL has destroyed all the sessions that never recevied a connection
		usleep(500 * 1000);

		MYSQL_QUERY(proxysql_admin, "SELECT * FROM stats_mysql_hostgroups_latencies WHERE hostgroup=1");
		MYSQL_RES* latencies_res = mysql_store_result(proxysql_admin);
		map<string, vector<string>> row_map = fetch_row_values(latencies_res);
		mysql_free_result(latencies_res);

		bool conns_timed_out = std::accumulate(
			query_err_codes.begin(), query_err_codes.end(), true,
			[](bool res, int res_code) { return res == true && res_code == 9001; }
		);

		uint32_t sessions_waited = std::stoll(row_map["sessions_waited"][0], NULL, 10);
		uint32_t sessions_waited_time_total = std::stoll(row_map["sessions_waited_time_total"][0], NULL, 10);
		double avg_waiting_time = sessions_waited == 0 ? 0 :
			(sessions_waited_time_total / static_cast<double>(sessions_waited)) / pow(10,6);

		ok(
			avg_waiting_time > (CONNECT_TIMEOUT_SERVER_MAX / 1000.0) - EPSILON &&
			avg_waiting_time < (CONNECT_TIMEOUT_SERVER_MAX / 1000.0) + EPSILON,
			"Connections timed out and average waiting time should match the imposed 'CONNECT_TIMEOUT_SERVER_MAX':"
			"{ conns_timed_out: '%d', exp: '%d', act: '%f', epsilon: '%f' }",
			conns_timed_out, CONNECT_TIMEOUT_SERVER_MAX, avg_waiting_time, EPSILON
		);

		uint32_t sessions_waiting = std::stoll(row_map["sessions_waiting"][0], NULL, 10);
		uint32_t conns_total = std::stoll(row_map["conns_total"][0], NULL, 10);
		uint32_t queries_total = std::stoll(row_map["queries_total"][0], NULL, 10);

		ok(
			sessions_waiting == 0 && conns_total == 0 && queries_total == 0,
			"'sessions_waiting', 'conns_total' and 'queries_total' should be equal to '0':"
			" { sessions_waiting: '%d', conns_total: '%d', queries_total: '%d' }",
			sessions_waiting, conns_total, queries_total
		);

		MYSQL_QUERY(proxysql_admin, "LOAD MYSQL VARIABLES FROM DISK");
		MYSQL_QUERY(proxysql_admin, "LOAD MYSQL VARIABLES TO RUNTIME");
	}

	MYSQL_QUERY(proxysql_admin, "LOAD MYSQL SERVERS FROM DISK");
	MYSQL_QUERY(proxysql_admin, "LOAD MYSQL SERVERS TO RUNTIME");
	MYSQL_QUERY(proxysql_admin, "SELECT * FROM stats_mysql_hostgroups_latencies_reset");
	mysql_free_result(mysql_store_result(proxysql_admin));
	usleep(500 * 1000);

	// TEST 4.
	{
		const uint32_t CONN_NUM = 500;
		const uint32_t CONNECT_TIMEOUT_SERVER_MAX = 20000;
		const uint32_t MAX_CONNECTIONS = 300;
		const uint32_t SLEEP_TIME = 5;
		// NOTE: This number was kept big because with small connections numbers, the average time can deviate
		// sightly from the expected value.
		double EPSILON = 2;

		MYSQL_QUERY(
			proxysql_admin,
			string {"SET mysql-connect_timeout_server_max=" + std::to_string(CONNECT_TIMEOUT_SERVER_MAX)}.c_str()
		);

		MYSQL_QUERY(proxysql_admin, "SET mysql-poll_timeout=100");
		MYSQL_QUERY(proxysql_admin, "LOAD MYSQL VARIABLES TO RUNTIME");

		vector<MYSQL*> proxy_conns {};
		int conn_err = open_connections(cl, CONN_NUM, proxy_conns);
		if (conn_err) { return EXIT_FAILURE; }

		MYSQL_QUERY(
			proxysql_admin,
			string {"UPDATE mysql_servers SET max_connections=" + std::to_string(MAX_CONNECTIONS)}.c_str()
		);
		MYSQL_QUERY(proxysql_admin, "LOAD MYSQL SERVERS TO RUNTIME");
		usleep(500 * 1000);

		vector<std::thread> query_threads {};
		std::mutex query_res_mutex;
		vector<int> query_err_codes {};

		std::transform(
			proxy_conns.begin(), proxy_conns.end(), std::back_inserter(query_threads),
			[&query_res_mutex,&query_err_codes,SLEEP_TIME](MYSQL* conn) -> std::thread {
				return std::thread([&query_res_mutex,&query_err_codes,conn,SLEEP_TIME]() -> void {
					std::string query {
						"/* hostgroup=0 */ SELECT SLEEP(" + std::to_string(SLEEP_TIME) + ")"
					};
					mysql_query(conn, query.c_str());
					std::lock_guard<std::mutex> lock_guard(query_res_mutex);
					query_err_codes.push_back(mysql_errno(conn));
				});
			}
		);

		// Give some time after launching connections
		usleep(500 * 1000);

		MYSQL_QUERY(proxysql_admin, "SELECT * FROM stats_mysql_hostgroups_latencies WHERE hostgroup=0");
		MYSQL_RES* latencies_res = mysql_store_result(proxysql_admin);
		map<string, vector<string>> row_map = fetch_row_values(latencies_res);
		mysql_free_result(latencies_res);

		uint32_t sessions_waited = std::stoll(row_map["sessions_waited"][0], NULL, 10);
		uint32_t sessions_waiting = std::stoll(row_map["sessions_waiting"][0], NULL, 10);
		uint32_t conns_total = std::stoll(row_map["conns_total"][0], NULL, 10);
		uint32_t queries_total = std::stoll(row_map["queries_total"][0], NULL, 10);

		ok(
			(sessions_waiting == CONN_NUM - MAX_CONNECTIONS) && (sessions_waited == CONN_NUM - MAX_CONNECTIONS) &&
			conns_total == MAX_CONNECTIONS && queries_total == 0,
			"Check expected values for:"
			" { sessions_waiting: '%d', sessions_waited: '%d', conns_total': '%d', queries_total: '%d', CONN_NUM: '%d' }",
			sessions_waiting, sessions_waited, conns_total, queries_total, CONN_NUM
		);

		sleep(SLEEP_TIME + 1);

		MYSQL_QUERY(
			proxysql_admin,
			string {"UPDATE mysql_servers SET max_connections=" + std::to_string(MAX_CONNECTIONS + 100)}.c_str()
		);
		MYSQL_QUERY(proxysql_admin, "LOAD MYSQL SERVERS TO RUNTIME");

		std::for_each(query_threads.begin(), query_threads.end(), [](std::thread& th) -> void { th.join(); });
		std::for_each(proxy_conns.begin(), proxy_conns.end(), [](MYSQL* conn) -> void { mysql_close(conn); });

		// Check that all the queries succeeded because sessions received the connection from connection pool
		bool conns_succeed = std::accumulate(
			query_err_codes.begin(), query_err_codes.end(), true,
			[](bool res, int res_code) { return res == true && res_code == 0; }
		);

		// Give some extra time to ProxySQL for the sessions processing
		usleep(500 * 1000);

		MYSQL_QUERY(proxysql_admin, "SELECT * FROM stats_mysql_hostgroups_latencies WHERE hostgroup=0");
		latencies_res = mysql_store_result(proxysql_admin);
		row_map = fetch_row_values(latencies_res);
		mysql_free_result(latencies_res);

		sessions_waited = std::stoll(row_map["sessions_waited"][0], NULL, 10);
		uint32_t sessions_waited_time_total = std::stoll(row_map["sessions_waited_time_total"][0], NULL, 10);
		double avg_waiting_time = sessions_waited == 0 ? 0 :
			(sessions_waited_time_total / static_cast<double>(sessions_waited)) / pow(10,6);

		ok(
			conns_succeed && avg_waiting_time > SLEEP_TIME - EPSILON && avg_waiting_time < SLEEP_TIME + EPSILON,
			"Connections succeed and average waiting time should match explicit waited time:"
			" { conn_succeed: '%d', exp: '%d', act: '%f', epsilon: '%f' }",
			conns_succeed, SLEEP_TIME, avg_waiting_time, EPSILON
		);

		MYSQL_QUERY(proxysql_admin, "SELECT * FROM stats_mysql_hostgroups_latencies WHERE hostgroup=0");
		latencies_res = mysql_store_result(proxysql_admin);
		row_map = fetch_row_values(latencies_res);
		mysql_free_result(latencies_res);

		sessions_waited = std::stoll(row_map["sessions_waited"][0], NULL, 10);
		sessions_waiting = std::stoll(row_map["sessions_waiting"][0], NULL, 10);
		conns_total = std::stoll(row_map["conns_total"][0], NULL, 10);
		queries_total = std::stoll(row_map["queries_total"][0], NULL, 10);

		ok(
			sessions_waiting == 0 && (sessions_waited == CONN_NUM - MAX_CONNECTIONS) &&
			conns_total == CONN_NUM && queries_total == CONN_NUM,
			"Check expected values for: "
			" { sessions_waiting: '%d', sessions_waited: '%d', conns_total': '%d', queries_total: '%d', CONN_NUM: '%d' }",
			sessions_waiting, sessions_waited, conns_total, queries_total, CONN_NUM
		);

		MYSQL_QUERY(proxysql_admin, "LOAD MYSQL VARIABLES FROM DISK");
		MYSQL_QUERY(proxysql_admin, "LOAD MYSQL VARIABLES TO RUNTIME");
	}

	mysql_close(proxysql_admin);

	return exit_status();
}