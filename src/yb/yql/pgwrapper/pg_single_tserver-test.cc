// Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//

#include "yb/client/client.h"
#include "yb/client/yb_table_name.h"

#include "yb/common/colocated_util.h"

#include "yb/tablet/tablet.h"
#include "yb/tablet/tablet_peer.h"
#include "yb/tablet/transaction_participant.h"

#include "yb/tserver/mini_tablet_server.h"
#include "yb/tserver/tablet_server.h"
#include "yb/tserver/ts_tablet_manager.h"

#include "yb/util/countdown_latch.h"
#include "yb/util/hdr_histogram.h"
#include "yb/util/metrics_writer.h"
#include "yb/util/metrics.h"
#include "yb/util/range.h"
#include "yb/util/stopwatch.h"
#include "yb/util/string_util.h"
#include "yb/util/test_thread_holder.h"
#include "yb/util/to_stream.h"

#include "yb/yql/pggate/pggate_flags.h"
#include "yb/yql/pgwrapper/pg_mini_test_base.h"

DEFINE_test_flag(int32, scan_tests_num_rows, 0,
                 "Number of rows to load for various scanning tests, or 0 for default.");

DECLARE_uint64(max_clock_skew_usec);
DECLARE_uint64(TEST_inject_sleep_before_applying_intents_ms);
DECLARE_bool(rocksdb_use_logging_iterator);
DECLARE_bool(ysql_enable_packed_row);
DECLARE_bool(ysql_enable_packed_row_for_colocated_table);
DECLARE_int64(global_memstore_size_mb_max);
DECLARE_int64(db_block_cache_size_bytes);

METRIC_DECLARE_histogram(handler_latency_yb_tserver_TabletServerService_Read);
METRIC_DECLARE_histogram(handler_latency_yb_tserver_TabletServerService_Write);
METRIC_DECLARE_entity(table);

DEFINE_RUNTIME_int32(TEST_scan_reads, 3, "Number of reads in scan tests");

namespace yb {

extern int TEST_scan_trivial_expectation;

namespace pgwrapper {

class PgSingleTServerTest : public PgMiniTestBase {
 protected:
  static constexpr const char* kDatabaseName = "testdb";

  size_t NumTabletServers() override {
    return 1;
  }

  void SetupColocatedTableAndRunBenchmark(
      const std::string& create_table_cmd, const std::string& insert_cmd,
      const std::string& select_cmd, int rows, int block_size, int reads, bool compact,
      bool aggregate) {
    auto conn = ASSERT_RESULT(Connect());

    ASSERT_OK(conn.ExecuteFormat("CREATE DATABASE $0 with COLOCATION = true", kDatabaseName));

    conn = ASSERT_RESULT(ConnectToDB(kDatabaseName));

    ASSERT_OK(conn.Execute(create_table_cmd));

    {
      auto write_histogram =
          cluster_->mini_tablet_server(0)->metric_entity().FindOrCreateMetric<Histogram>(
              &METRIC_handler_latency_yb_tserver_TabletServerService_Write)->underlying();
      auto metric_start = write_histogram->TotalSum();
      auto start = MonoTime::Now();
      auto last_row = 0;
      while (last_row < rows) {
        auto first_row = last_row + 1;
        last_row = std::min(rows, last_row + block_size);
        ASSERT_OK(conn.ExecuteFormat(insert_cmd, first_row, last_row));
      }
      auto finish = MonoTime::Now();
      auto metric_finish = write_histogram->TotalSum();
      LOG(INFO) << "Insert time: " << finish - start << ", tserver time: "
                << MonoDelta::FromMicroseconds(metric_finish - metric_start);
    }

    auto peers = ListTabletPeers(cluster_.get(), ListPeersFilter::kAll);
    for (const auto& peer : peers) {
      auto tp = peer->tablet()->transaction_participant();
      if (tp) {
        const auto count_intents_result = tp->TEST_CountIntents();
        const auto count_intents =
            count_intents_result.ok() ? count_intents_result->num_intents : 0;
        LOG(INFO) << peer->LogPrefix() << "Intents: " << count_intents;
      }
    }

    if (select_cmd.empty()) {
      LOG(INFO) << "Skipping read workload";
      return;
    }

    if (compact) {
      FlushAndCompactTablets();
    }

    LOG(INFO) << "Perform read. Row count: " << rows;

    if (VLOG_IS_ON(4)) {
      google::SetVLOGLevel("intent_aware_iterator", 4);
      google::SetVLOGLevel("docdb_rocksdb_util", 4);
      google::SetVLOGLevel("docdb", 4);
    }

    auto read_histogram =
        cluster_->mini_tablet_server(0)->metric_entity().FindOrCreateMetric<Histogram>(
            &METRIC_handler_latency_yb_tserver_TabletServerService_Read)->underlying();

    for (int i = 0; i != reads; ++i) {
      int64_t fetched_rows;
      auto metric_start = read_histogram->TotalSum();
      auto start = MonoTime::Now();
      if (aggregate) {
        fetched_rows = ASSERT_RESULT(conn.FetchRow<PGUint64>(select_cmd));
      } else {
        auto res = ASSERT_RESULT(conn.Fetch(select_cmd));
        fetched_rows = PQntuples(res.get());
      }
      auto finish = MonoTime::Now();
      auto metric_finish = read_histogram->TotalSum();
      ASSERT_EQ(rows, fetched_rows);
      LOG(INFO) << i << ") Full Time: " << finish - start
                << ", tserver time: " << MonoDelta::FromMicroseconds(metric_finish - metric_start);
    }
  }

  tserver::TabletServer& tablet_server() {
    return *cluster_->mini_tablet_server(0)->server();
  }

  Result<std::string> GetColocatedTableId() {
    if (!colocated_table_id_.empty()) {
      return colocated_table_id_;
    }
    auto yb_client = VERIFY_RESULT(client::YBClientBuilder()
        .add_master_server_addr(cluster_->GetMasterAddresses())
        .default_admin_operation_timeout(MonoDelta::FromSeconds(60))
        .Build(tablet_server().messenger()));
    auto tables = VERIFY_RESULT(yb_client->ListTables(/* filter= */ "", /* exclude_ysql= */ false));
    std::string colocated_table_id;
    for (const auto& table : tables) {
      if (StringEndsWith(table.table_id(), kColocationParentTableIdSuffix)) {
        colocated_table_id = table.table_id();
        break;
      }
    }
    if (colocated_table_id.empty()) {
      return STATUS(IllegalState, "Could not identify colocated table id");
    }
    colocated_table_id_ = colocated_table_id;
    return colocated_table_id;
  }

  Result<std::pair<int64_t, int64_t>> GetBlockCacheHitMissCounts() {
    auto* metric_registry = tablet_server().tablet_manager()->TEST_metric_registry();

    std::stringstream out;

    MetricPrometheusOptions opts;
    PrometheusWriter writer(&out, opts);

    RETURN_NOT_OK(metric_registry->WriteForPrometheus(&writer, opts));
    auto lines = StringSplit(out.str(), '\n');
    int64_t block_cache_hit_count = -1;
    int64_t block_cache_miss_count = -1;
    auto colocated_table_id = VERIFY_RESULT(GetColocatedTableId());
    auto try_parse_metric =
        [](const char* prefix, const std::string& line, int64_t& result) -> Status {
          if (!StringStartsWithOrEquals(line, prefix)) {
            return Status::OK();
          }
          auto items = StringSplit(line, ' ');
          if (items.size() < 3) {
            return Status::OK();
          }
          int64_t number = -1;
          try {
            number = std::stoll(items[items.size() - 2]);
          } catch (const std::exception& exc) {
            return STATUS_FORMAT(InvalidArgument, "Error parsing metric from line: $0", line);
          }
          if (result != -1) {
            return STATUS_FORMAT(
                IllegalState,
                "Duplicate values for metric with prefix '$0': $1 vs $2",
                prefix, result, number);
          }
          result = number;
          return Status::OK();
        };
    for (const auto& line : lines) {
      if (line.find(colocated_table_id) != std::string::npos) {
        RETURN_NOT_OK(
            try_parse_metric("rocksdb_block_cache_data_hit{", line, block_cache_hit_count));
        RETURN_NOT_OK(
            try_parse_metric("rocksdb_block_cache_data_miss{", line, block_cache_miss_count));
      }
    }
    LOG(INFO) << "Data block cache hit count: " << block_cache_hit_count
              << ", miss count: " << block_cache_miss_count;
    return std::make_pair(block_cache_hit_count, block_cache_miss_count);
  }

 private:
  std::string colocated_table_id_;
};

TEST_F(PgSingleTServerTest, ManyRowsInsert) {
  constexpr int kRows = RegularBuildVsDebugVsSanitizers(100000, 10000, 1000);
  auto conn = ASSERT_RESULT(Connect());

  ASSERT_OK(conn.Execute("CREATE TABLE t (key INT PRIMARY KEY)"));

  auto start = MonoTime::Now();
  ASSERT_OK(conn.ExecuteFormat("INSERT INTO t SELECT generate_series(1, $0)", kRows));
  auto finish = MonoTime::Now();
  LOG(INFO) << "Time: " << finish - start;
}

class PgMiniBigPrefetchTest : public PgSingleTServerTest {
 public:
  Status SetupConnection(PGConn* conn) const override {
    return conn->Execute("SET yb_fetch_row_limit = 20000000");
  }

  void Run(int rows, int block_size, int reads, bool compact = false, bool select = false) {
    const std::string create_cmd = "CREATE TABLE t (a int PRIMARY KEY)";
    const std::string insert_cmd = "INSERT INTO t SELECT generate_series($0, $1)";
    const std::string select_cmd = select ? "SELECT * FROM t" : "SELECT count(*) FROM t";
    SetupColocatedTableAndRunBenchmark(
        create_cmd, insert_cmd, select_cmd, rows, block_size, reads, compact,
        /* aggregate = */ !select);
  }
};

class PgMiniSmallMemstoreAndCacheTest : public PgSingleTServerTest {
  void OverrideMiniClusterOptions(MiniClusterOptions* options) override {
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_global_memstore_size_mb_max) = 16;
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_db_block_cache_size_bytes) = 32 * 1024 * 1024;
  }
};

TEST_F_EX(PgSingleTServerTest, BigRead, PgMiniBigPrefetchTest) {
  constexpr int kRows = RegularBuildVsDebugVsSanitizers(1000000, 100000, 10000);
  constexpr int kBlockSize = 1000;
  constexpr int kReads = 3;

  Run(kRows, kBlockSize, kReads);
}

TEST_F_EX(PgSingleTServerTest, BigReadWithCompaction,
          PgMiniBigPrefetchTest) {
  constexpr int kRows = RegularBuildVsDebugVsSanitizers(1000000, 100000, 10000);
  constexpr int kBlockSize = 1000;
  constexpr int kReads = 3;

  Run(kRows, kBlockSize, kReads, /* compact= */ true);
}

TEST_F_EX(PgSingleTServerTest, SmallRead, PgMiniBigPrefetchTest) {
  constexpr int kRows = 10;
  constexpr int kBlockSize = kRows;
  constexpr int kReads = 1;

  Run(kRows, kBlockSize, kReads);
}

namespace {

constexpr int kReleaseNumScanRows = 1000000;
constexpr int kDebugNumScanRows = 100000;
constexpr int kSanitizerNumScanRows = 10000;

int NumScanRows() {
  auto n = FLAGS_TEST_scan_tests_num_rows;
  if (!n) {
    n = RegularBuildVsDebugVsSanitizers(
        kReleaseNumScanRows, kDebugNumScanRows, kSanitizerNumScanRows);
  }
  CHECK_GE(n, 0);
  return n;
}

constexpr int kScanBlockSize = 1000;

std::string CreateTableWithNValuesCommand(int num_columns, bool add_pk = true) {
  std::string result = "CREATE TABLE t (";
  if (add_pk) {
    result += "a int PRIMARY KEY";
  }
  for (auto column : Range(num_columns)) {
    if (add_pk || column != 0) {
      result += ", ";
    }
    result += Format("c$0 INT", column);
  }
  result += ")";
  return result;
}

std::string InsertNValuesCommand(int num_columns) {
  std::string result = "INSERT INTO t VALUES (generate_series($0, $1)";
  for (int i = 0; i != num_columns; ++i) {
    result += ", trunc(random()*100000000)";
  }
  result += ")";
  return result;
}

} // namespace

TEST_F_EX(PgSingleTServerTest, Scan, PgMiniBigPrefetchTest) {
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_ysql_enable_packed_row) = false;
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_ysql_enable_packed_row_for_colocated_table) = false;
  Run(NumScanRows(), kScanBlockSize, FLAGS_TEST_scan_reads, /* compact= */ false,
      /* select= */ true);
}

TEST_F_EX(PgSingleTServerTest, ScanWithPackedRow, PgMiniBigPrefetchTest) {
  constexpr int kNumColumns = 10;

  ANNOTATE_UNPROTECTED_WRITE(FLAGS_ysql_enable_packed_row) = true;
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_ysql_enable_packed_row_for_colocated_table) = true;

  auto create_cmd = CreateTableWithNValuesCommand(kNumColumns);
  auto insert_cmd = InsertNValuesCommand(kNumColumns);
  const std::string select_cmd = "SELECT * FROM t";
  SetupColocatedTableAndRunBenchmark(
      create_cmd, insert_cmd, select_cmd, NumScanRows(), kScanBlockSize, FLAGS_TEST_scan_reads,
      /* compact= */ false, /* aggregate= */ false);
}

TEST_F_EX(PgSingleTServerTest, YB_DISABLE_TEST_ON_MACOS(HybridTimeFilterDuringConflictResolution),
          PgMiniSmallMemstoreAndCacheTest) {
  constexpr int kNumColumns = 10;

  ANNOTATE_UNPROTECTED_WRITE(FLAGS_ysql_enable_packed_row) = true;
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_ysql_enable_packed_row_for_colocated_table) = true;

  auto create_cmd = CreateTableWithNValuesCommand(kNumColumns);
  auto insert_cmd = InsertNValuesCommand(kNumColumns);
  SetupColocatedTableAndRunBenchmark(
      create_cmd, insert_cmd, /* select_cmd= */ "", NumScanRows(), kScanBlockSize,
      FLAGS_TEST_scan_reads, /* compact= */ false, /* aggregate= */ false);
  auto [block_cache_hit_count, block_cache_miss_count] =
      ASSERT_RESULT(GetBlockCacheHitMissCounts());
  ASSERT_GE(block_cache_hit_count, 0);
  ASSERT_GE(block_cache_miss_count, 0);
  if (NumScanRows() == kReleaseNumScanRows) {
    LOG(INFO) << "Checking that block cache hit/miss counts are within expected ranges";
    ASSERT_GE(block_cache_miss_count, 9000);
    ASSERT_LE(block_cache_miss_count, 11000);
    // The hit count would be ~30000 with docdb_ht_filter_conflict_with_committed turned off.
    ASSERT_GE(block_cache_hit_count, 14000);
    ASSERT_LE(block_cache_hit_count, 18000);
  } else {
    LOG(INFO) << "The number of rows " << NumScanRows() << " is different from the release build "
              << "number of rows " << kReleaseNumScanRows << ", not checking block cache stats.";
  }
}

TEST_F_EX(PgSingleTServerTest, ScanWithLowerLimit, PgMiniBigPrefetchTest) {
  constexpr int kNumColumns = 5;

  auto create_cmd = CreateTableWithNValuesCommand(kNumColumns);
  auto insert_cmd = InsertNValuesCommand(kNumColumns);
  const std::string select_cmd = "SELECT * FROM t WHERE a > 0";
  SetupColocatedTableAndRunBenchmark(
      create_cmd, insert_cmd, select_cmd, NumScanRows(), kScanBlockSize, FLAGS_TEST_scan_reads,
      /* compact= */ false, /* aggregate = */ false);
}

TEST_F_EX(PgSingleTServerTest, IndexScan, PgMiniBigPrefetchTest) {
  constexpr int kNumColumns = 2;

  auto create_cmd = CreateTableWithNValuesCommand(kNumColumns, false) + ";";
  create_cmd += "CREATE INDEX ON t (c0 ASC);";
  auto insert_cmd = InsertNValuesCommand(kNumColumns - 1);
  const std::string select_cmd = "SELECT c0 FROM t WHERE c0 > 0";
  SetupColocatedTableAndRunBenchmark(
      create_cmd, insert_cmd, select_cmd, NumScanRows() / 2, kScanBlockSize, FLAGS_TEST_scan_reads,
      /* compact= */ false, /* aggregate = */ false);
}

TEST_F_EX(PgSingleTServerTest, ScanWithCompaction, PgMiniBigPrefetchTest) {
  Run(NumScanRows(), kScanBlockSize, FLAGS_TEST_scan_reads, /* compact= */ true,
      /* select= */ true);
}

TEST_F_EX(PgSingleTServerTest, ScanSkipPK, PgMiniBigPrefetchTest) {
  const auto num_rows = NumScanRows() / 2;
  constexpr int kNumKeyColumns = 5;

  ANNOTATE_UNPROTECTED_WRITE(FLAGS_ysql_enable_packed_row) = true;
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_ysql_enable_packed_row_for_colocated_table) = true;

  std::string create_cmd = "CREATE TABLE t (";
  std::string pk = "";
  std::string insert_cmd = "INSERT INTO t VALUES (";
  for (const auto column : Range(kNumKeyColumns)) {
    create_cmd += Format("r$0 TEXT, ", column);
    if (!pk.empty()) {
      pk += ", ";
    }
    pk += Format("r$0", column);
    insert_cmd += "MD5(random()::text), ";
  }
  create_cmd += "value INT, PRIMARY KEY (" + pk + "))";
  insert_cmd += "generate_series($0, $1))";
  const std::string select_cmd = "SELECT value FROM t";
  SetupColocatedTableAndRunBenchmark(
      create_cmd, insert_cmd, select_cmd, num_rows, kScanBlockSize, FLAGS_TEST_scan_reads,
      /* compact= */ false, /* aggregate = */ false);
}

TEST_F_EX(PgSingleTServerTest, ScanBigPK, PgMiniBigPrefetchTest) {
  const auto num_rows = NumScanRows() / 4;
  constexpr auto kNumRepetitions = 10;

  ANNOTATE_UNPROTECTED_WRITE(FLAGS_ysql_enable_packed_row) = true;
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_ysql_enable_packed_row_for_colocated_table) = true;

  std::string create_cmd = "CREATE TABLE t (k TEXT PRIMARY KEY, value INT)";
  std::string insert_cmd = "INSERT INTO t (k, value) VALUES (";
  for (const auto i : Range(kNumRepetitions)) {
    if (i) {
      insert_cmd += "||";
    }
    insert_cmd += "md5(random()::TEXT)";
  }
  insert_cmd += ", generate_series($0, $1))";
  const std::string select_cmd = "SELECT k FROM t";
  SetupColocatedTableAndRunBenchmark(
      create_cmd, insert_cmd, select_cmd, num_rows, kScanBlockSize, FLAGS_TEST_scan_reads,
      /* compact= */ false, /* aggregate = */ false);
}

TEST_F_EX(PgSingleTServerTest, ScanComplexPK, PgMiniBigPrefetchTest) {
  const auto num_rows = NumScanRows() / 2;
  constexpr int kNumKeyColumns = 10;

  ANNOTATE_UNPROTECTED_WRITE(FLAGS_ysql_enable_packed_row) = true;
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_ysql_enable_packed_row_for_colocated_table) = true;

  std::string create_cmd = "CREATE TABLE t (";
  std::string pk = "";
  std::string insert_cmd = "INSERT INTO t VALUES (generate_series($0, $1)";
  for (const auto column : Range(kNumKeyColumns)) {
    create_cmd += Format("r$0 INT, ", column);
    if (!pk.empty()) {
      pk += ", ";
    }
    pk += Format("r$0", column);
    insert_cmd += ", trunc(random()*100000000)";
  }
  create_cmd += "value INT, PRIMARY KEY (" + pk + "))";
  insert_cmd += ")";
  const std::string select_cmd = "SELECT * FROM t";
  SetupColocatedTableAndRunBenchmark(
      create_cmd, insert_cmd, select_cmd, num_rows, kScanBlockSize, FLAGS_TEST_scan_reads,
      /* compact= */ false, /* aggregate = */ false);
}

TEST_F_EX(PgSingleTServerTest, ScanSkipValues, PgMiniBigPrefetchTest) {
  const auto num_rows = NumScanRows() / 4;
  constexpr auto kNumExtraColumns = 10;

  ANNOTATE_UNPROTECTED_WRITE(FLAGS_ysql_enable_packed_row) = true;
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_ysql_enable_packed_row_for_colocated_table) = true;

  std::string create_cmd = "CREATE TABLE t (k INT PRIMARY KEY, value INT";
  std::string insert_cmd = "INSERT INTO t (k, value";
  std::string insert_cmd_suffix;
  for (const auto i : Range(kNumExtraColumns)) {
    create_cmd += Format(", extra_value$0 TEXT", i);
    insert_cmd += Format(", extra_value$0", i);
    insert_cmd_suffix += ", md5(random()::TEXT)";
  }
  create_cmd += ")";
  insert_cmd += ") VALUES (generate_series($0, $1), trunc(random()*100000000)";
  insert_cmd += insert_cmd_suffix;
  insert_cmd += ")";
  const std::string select_cmd = "SELECT value FROM t";
  SetupColocatedTableAndRunBenchmark(
      create_cmd, insert_cmd, select_cmd, num_rows, kScanBlockSize, FLAGS_TEST_scan_reads,
      /* compact= */ false, /* aggregate = */ false);
}


TEST_F(PgSingleTServerTest, BigValue) {
  constexpr size_t kValueSize = 32_MB;
  constexpr int kKey = 42;
  const std::string kValue = RandomHumanReadableString(kValueSize);

  auto conn = ASSERT_RESULT(Connect());
  ASSERT_OK(conn.Execute("CREATE TABLE t (a int PRIMARY KEY, b TEXT) SPLIT INTO 1 TABLETS"));
  ASSERT_OK(conn.ExecuteFormat("INSERT INTO t VALUES ($0, '$1')", kKey, kValue));

  auto start = MonoTime::Now();
  auto result = ASSERT_RESULT(conn.FetchRow<std::string>(
      Format("SELECT md5(b) FROM t WHERE a = $0", kKey)));
  auto finish = MonoTime::Now();
  LOG(INFO) << "Passed: " << finish - start << ", result: " << result;
}

class PgSmallPrefetchTest : public PgSingleTServerTest {
 protected:
  Status SetupConnection(PGConn* conn) const override {
    return conn->Execute("SET yb_fetch_row_limit = 1");
  }

  void Run(int rows, int block_size, int reads) {
    const std::string create_cmd = "CREATE TABLE t (a int PRIMARY KEY)";
    const std::string insert_cmd = "INSERT INTO t SELECT generate_series($0, $1)";
    const std::string select_cmd = "SELECT * from t where a in (SELECT generate_series(1, 10000))";
    SetupColocatedTableAndRunBenchmark(
        create_cmd, insert_cmd, select_cmd, rows, block_size, reads, /* compact = */ true,
        /* aggregate = */ false);
  }
};

TEST_F_EX(PgSingleTServerTest, SingleRowScan, PgSmallPrefetchTest) {
  constexpr int kRows = RegularBuildVsDebugVsSanitizers(10000, 1000, 100);
  constexpr int kBlockSize = 100;
  constexpr int kReads = 3;

  Run(kRows, kBlockSize, kReads);
}

TEST_F_EX(
    PgSingleTServerTest, TestPagingInSerializableIsolation,
    PgSmallPrefetchTest) {
  // This test is related to #14284, #13041. As part of a regression, the read time set in the
  // paging state returned by the tserver to YSQL, was sent back by YSQL in subsequent read
  // requests even for serializable isolation level. This is only correct for the other isolation
  // levels. In serializable isolation level, a read time is invalid since each read is supposed to
  // read and lock the latest data. This resulted in the tserver crashing with -
  // "Read time should NOT be specified for serializable isolation level".
  auto conn = ASSERT_RESULT(Connect());
  ASSERT_OK(conn.Execute("CREATE TABLE test (k INT PRIMARY KEY, v INT) SPLIT INTO 1 TABLETS"));
  ASSERT_OK(conn.Execute("INSERT INTO test SELECT GENERATE_SERIES(1, 10)"));

  ASSERT_OK(conn.Execute("BEGIN TRANSACTION ISOLATION LEVEL SERIALIZABLE"));
  ASSERT_OK(conn.Execute("DECLARE c CURSOR FOR SELECT * FROM test"));
  ASSERT_OK(conn.Fetch("FETCH c"));
  ASSERT_OK(conn.Fetch("FETCH c"));
  ASSERT_OK(conn.Execute("COMMIT"));

  ASSERT_OK(conn.Execute("BEGIN TRANSACTION ISOLATION LEVEL SERIALIZABLE"));
  ASSERT_OK(conn.Fetch("SELECT * FROM test"));
  ASSERT_OK(conn.Execute("COMMIT"));
}

TEST_F_EX(
    PgSingleTServerTest, TestDeferrablePagingInSerializableIsolation,
    PgSmallPrefetchTest) {
  // Caution: this number of rows is much smaller than that in most other tests in this file.
  const auto num_rows = 4;
  auto conn = ASSERT_RESULT(Connect());
  ASSERT_OK(conn.Execute("CREATE TABLE test (key INT PRIMARY KEY, v INT)"));
  for (auto i = 0; i < num_rows; ++i) {
    ASSERT_OK(conn.ExecuteFormat("INSERT INTO test VALUES ($0, $1)", i, 0));
  }

  constexpr auto kWriteNumIterations = 1000u;

  CountDownLatch sync_start_latch(2);
  std::atomic<size_t> num_write_iterations{0};
  TestThreadHolder thread_holder;
  thread_holder.AddThreadFunctor(
      [this, &stop = thread_holder.stop_flag(), &num_write_iterations, &sync_start_latch]() {
        auto write_conn = ASSERT_RESULT(Connect());
        sync_start_latch.CountDown();
        sync_start_latch.Wait();
        while (!stop.load(std::memory_order_acquire)) {
          ASSERT_OK(write_conn.Execute("UPDATE test SET v=0"));
          ASSERT_OK(write_conn.Execute("UPDATE test SET v=1"));
          num_write_iterations.fetch_add(2, std::memory_order_acq_rel);
        }
      });

  // Since each DEFERRABLE waits for max_clock_skew_usec, set it to a low value to avoid a
  // very large test time.
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_max_clock_skew_usec) = 2000;

  constexpr auto kReadNumIterations = 1000u;
  auto read_conn = ASSERT_RESULT(Connect());
  sync_start_latch.CountDown();
  sync_start_latch.Wait();
  for (auto i = 0u; i < kReadNumIterations ||
          num_write_iterations.load(std::memory_order_acquire) < kWriteNumIterations; ++i) {
    ASSERT_OK(read_conn.Execute(
        "BEGIN TRANSACTION ISOLATION LEVEL SERIALIZABLE, READ ONLY, DEFERRABLE"));

    auto res = ASSERT_RESULT(read_conn.FetchMatrix("SELECT v FROM test", num_rows, 1));

    // Ensure that all rows in the table have the same value.
    auto common_value_for_all_rows = ASSERT_RESULT(GetValue<int32_t>(res.get(), 0, 0));
    for (auto i = 1; i < num_rows; ++i) {
      ASSERT_EQ(common_value_for_all_rows, ASSERT_RESULT(GetValue<int32_t>(res.get(), i, 0)));
    }
    ASSERT_OK(read_conn.Execute("COMMIT"));
  }
}

// Microbenchmark, see
// https://github.com/yugabyte/benchbase/blob/main/config/yugabyte/scan_workloads/yb_colocated/
// scanG7_colo_pkey_rangescan_fullTableScan_increasingColumn.yaml
TEST_F(PgSingleTServerTest, YB_DISABLE_TEST(PerfScanG7RangePK100Columns)) {
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_ysql_enable_packed_row) = true;
    ANNOTATE_UNPROTECTED_WRITE(FLAGS_ysql_enable_packed_row_for_colocated_table) = true;

  constexpr auto kNumColumns = 100;
  const auto num_rows = RegularBuildVsDebugVsSanitizers(10'000, 1000, 100);
  constexpr auto kNumScansPerIteration = 10;
  constexpr auto kNumIterations = 3;

  auto conn = ASSERT_RESULT(Connect());
  ASSERT_OK(conn.ExecuteFormat("CREATE DATABASE $0 with colocated=true", kDatabaseName));
  conn = ASSERT_RESULT(ConnectToDB(kDatabaseName));

  {
    std::string create_stmt = "CREATE TABLE t(col_bigint_id_1 bigint,";
    for (int i = 1; i <= kNumColumns; ++i) {
      create_stmt += Format("col_bigint_$0 bigint, ", i);
    }
    create_stmt += "PRIMARY KEY(col_bigint_id_1 ASC));";
    ASSERT_OK(conn.Execute(create_stmt));
  }

  ASSERT_OK(
      conn.Execute("CREATE FUNCTION random_between(low INT, high INT) RETURNS INT AS \n"
                   "$$\n"
                   "BEGIN\n"
                   "   RETURN floor(random()*(high-low+1)+low);\n"
                   "END;\n"
                   "$$ LANGUAGE plpgsql STRICT;"));

  {
    LOG(INFO) << "Loading data...";

    Stopwatch s(Stopwatch::ALL_THREADS);
    s.start();

    std::string load_stmt = "INSERT INTO t SELECT i";
    for (int i = 1; i <= kNumColumns; ++i) {
      load_stmt += ", random_between(1, 1000000)";
    }
    load_stmt += Format(" FROM generate_series(1, $0) as i;", num_rows);
    ASSERT_OK(conn.ExecuteFormat(load_stmt));

    s.stop();
    LOG(INFO) << "Load took: " << AsString(s.elapsed());
  }

  FlushAndCompactTablets();

  const auto rows_inserted = ASSERT_RESULT(conn.FetchRow<int64_t>("SELECT COUNT(*) FROM t"));
  LOG(INFO) << "Rows inserted: " << rows_inserted;
  ASSERT_EQ(rows_inserted, num_rows);

  for (int i = 0; i < kNumIterations; ++i) {
    Stopwatch s(Stopwatch::ALL_THREADS);
    s.start();

    for (int j = 0; j < kNumScansPerIteration; ++j) {
      auto res = ASSERT_RESULT(conn.Fetch("SELECT * FROM t WHERE col_bigint_id_1>1"));
      ASSERT_EQ(PQntuples(res.get()), num_rows - 1);
    }

    s.stop();
    LOG(INFO) << kNumScansPerIteration << " scan(s) took: " << AsString(s.elapsed());
  }
}

TEST_F_EX(PgSingleTServerTest, ColocatedJoinPerformance,
          PgSmallPrefetchTest) {
  constexpr int kNumRows = RegularBuildVsDebugVsSanitizers(10000, 1000, 100);
  auto conn = ASSERT_RESULT(Connect());

  ASSERT_OK(conn.ExecuteFormat("CREATE DATABASE $0 with colocated=true", kDatabaseName));

  conn = ASSERT_RESULT(ConnectToDB(kDatabaseName));

  ASSERT_OK(conn.Execute("CREATE TABLE t1(k INT PRIMARY KEY, v1 INT)"));
  ASSERT_OK(conn.Execute("CREATE TABLE t2(k INT PRIMARY KEY, v2 INT)"));
  ASSERT_OK(conn.ExecuteFormat(
      "INSERT INTO t2 SELECT s, s FROM generate_series(1, $0) AS s", kNumRows));
  ASSERT_OK(conn.ExecuteFormat(
      "INSERT INTO t1 SELECT s, s FROM generate_series(1, $0) AS s", kNumRows));

  auto start = MonoTime::Now();
  auto res = ASSERT_RESULT(conn.FetchRow<int32_t>(
      "SELECT v1 + v2 FROM t1 INNER JOIN t2 ON (t1.k = t2.k) WHERE v2 < 2 OR v1 < 2"));
  auto finish = MonoTime::Now();
  ASSERT_EQ(res, 2);
  LOG(INFO) << "Time: " << finish - start;
}

// ------------------------------------------------------------------------------------------------
// Backward scan on an index
// ------------------------------------------------------------------------------------------------

class PgBackwardIndexScanTest : public PgSingleTServerTest {
 protected:
  void BackwardIndexScanTest(bool uncommitted_intents) {
    auto conn = ASSERT_RESULT(Connect());

    ASSERT_OK(conn.Execute(R"#(
        create table events_backwardscan (

          log       text not null,
          src       text not null,
          inserted  timestamp(3) without time zone not null,
          created   timestamp(3) without time zone not null,
          data      jsonb not null,

          primary key (log, src, created)
        );
      )#"));
    ASSERT_OK(conn.Execute("create index on events_backwardscan (inserted asc);"));

    for (int day = 1; day <= 31; ++day) {
      ASSERT_OK(conn.ExecuteFormat(R"#(
          insert into events_backwardscan

          select
            'log',
            'src',
            t,
            t,
            '{}'

          from generate_series(
            timestamp '2020-01-$0 00:00:00',
            timestamp '2020-01-$0 23:59:59',
            interval  '1 minute'
          )

          as t(day);
      )#", day));
    }

    std::optional<PGConn> uncommitted_intents_conn;
    if (uncommitted_intents) {
      uncommitted_intents_conn = ASSERT_RESULT(Connect());
      ASSERT_OK(uncommitted_intents_conn->Execute("BEGIN"));
      auto ts = "1970-01-01 00:00:00";
      ASSERT_OK(uncommitted_intents_conn->ExecuteFormat(
          "insert into events_backwardscan values ('log', 'src', '$0', '$0', '{}')", ts, ts));
    }

    auto count = ASSERT_RESULT(
        conn.FetchRow<PGUint64>("SELECT COUNT(*) FROM events_backwardscan"));
    LOG(INFO) << "Total rows inserted: " << count;

    auto select_result = ASSERT_RESULT(conn.Fetch(
        "select * from events_backwardscan order by inserted desc limit 100"
    ));
    ASSERT_EQ(PQntuples(select_result.get()), 100);

    if (uncommitted_intents) {
      ASSERT_OK(uncommitted_intents_conn->Execute("ROLLBACK"));
    }
  }
};

TEST_F_EX(PgSingleTServerTest,
          BackwardIndexScanNoIntents,
          PgBackwardIndexScanTest) {
  BackwardIndexScanTest(/* uncommitted_intents */ false);
}

TEST_F_EX(PgSingleTServerTest,
          BackwardIndexScanWithIntents,
          PgBackwardIndexScanTest) {
  BackwardIndexScanTest(/* uncommitted_intents */ true);
}

class PgRocksDbIteratorLoggingTest : public PgSingleTServerTest {
 public:
  struct IteratorLoggingTestConfig {
    int num_non_pk_columns;
    int num_rows;
    int num_overwrites;
    int first_row_to_scan;
    int last_row_to_scan;
  };

  void RunIteratorLoggingTest(const IteratorLoggingTestConfig& config) {
    auto conn = ASSERT_RESULT(Connect());

    std::string non_pk_columns_schema;
    std::string non_pk_column_names;
    for (int i = 0; i < config.num_non_pk_columns; ++i) {
      non_pk_columns_schema += Format(", $0 TEXT", GetNonPkColName(i));
      non_pk_column_names += Format(", $0", GetNonPkColName(i));
    }
    ASSERT_OK(conn.ExecuteFormat("CREATE TABLE t (pk TEXT, PRIMARY KEY (pk ASC)$0)",
                                 non_pk_columns_schema));
    // Delete and overwrite every row multiple times.
    for (int overwrite_index = 0; overwrite_index < config.num_overwrites; ++overwrite_index) {
      for (int row_index = 0; row_index < config.num_rows; ++row_index) {
        std::string non_pk_values;
        for (int non_pk_col_index = 0;
             non_pk_col_index < config.num_non_pk_columns;
             ++non_pk_col_index) {
          non_pk_values += Format(", '$0'", GetNonPkColValue(
              non_pk_col_index, row_index, overwrite_index));
        }

        const auto pk_value = GetPkForRow(row_index);
        ASSERT_OK(conn.ExecuteFormat(
            "INSERT INTO t(pk$0) VALUES('$1'$2)", non_pk_column_names, pk_value, non_pk_values));
        if (overwrite_index != config.num_overwrites - 1) {
          ASSERT_OK(conn.ExecuteFormat("DELETE FROM t WHERE pk = '$0'", pk_value));
        }
      }
    }
    const auto first_pk_to_scan = GetPkForRow(config.first_row_to_scan);
    const auto last_pk_to_scan = GetPkForRow(config.last_row_to_scan);
    auto count_stmt_str = Format(
        "SELECT COUNT(*) FROM t WHERE pk >= '$0' AND pk <= '$1'",
        first_pk_to_scan,
        last_pk_to_scan);
    // Do the same scan twice, and only turn on iterator logging on the second scan.
    // This way we won't be logging system table operations needed to fetch PostgreSQL metadata.
    for (bool is_warmup : {true, false}) {
      if (!is_warmup) {
        SetAtomicFlag(true, &FLAGS_rocksdb_use_logging_iterator);
      }
      auto actual_num_rows = ASSERT_RESULT(conn.FetchRow<PGUint64>(count_stmt_str));
      const int expected_num_rows = config.last_row_to_scan - config.first_row_to_scan + 1;
      ASSERT_EQ(expected_num_rows, actual_num_rows);
    }
    SetAtomicFlag(false, &FLAGS_rocksdb_use_logging_iterator);
  }

 private:
  std::string GetNonPkColName(int non_pk_col_index) {
    return Format("non_pk_col$0", non_pk_col_index);
  }

  std::string GetPkForRow(int row_index) {
    return Format("PrimaryKeyForRow$0", row_index);
  }

  std::string GetNonPkColValue(int non_pk_col_index, int row_index, int overwrite_index) {
    return Format("NonPkCol$0ValueForRow$1Overwrite$2",
                  non_pk_col_index, row_index, overwrite_index);
  }
};

TEST_F_EX(PgSingleTServerTest,
          IteratorLogPkOnly, PgRocksDbIteratorLoggingTest) {
  RunIteratorLoggingTest({
    .num_non_pk_columns = 0,
    .num_rows = 5,
    .num_overwrites = 100,
    .first_row_to_scan = 1,  // 0-based
    .last_row_to_scan = 3,
  });
}

TEST_F_EX(PgSingleTServerTest,
          IteratorLogTwoNonPkCols, PgRocksDbIteratorLoggingTest) {
  RunIteratorLoggingTest({
    .num_non_pk_columns = 2,
    .num_rows = 5,
    .num_overwrites = 100,
    .first_row_to_scan = 1,  // 0-based
    .last_row_to_scan = 3,
  });
}

// Repro for https://github.com/yugabyte/yugabyte-db/issues/17558.
TEST_F(PgSingleTServerTest, PagingSelectWithDelayedIntentsApply) {
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_inject_sleep_before_applying_intents_ms) = 100;
  auto conn = ASSERT_RESULT(Connect());
  ASSERT_OK(conn.Execute("CREATE TABLE t (v INT) SPLIT INTO 2 TABLETS"));
  for (int i = 0; i != 20; ++i) {
    LOG(INFO) << "Delete iteration " << i;
    ASSERT_OK(conn.Execute("DELETE FROM t"));
    LOG(INFO) << "Insert iteration " << i;
    ASSERT_OK(conn.Execute("INSERT INTO t VALUES (1)"));
    LOG(INFO) << "Reading iteration " << i;
    ASSERT_EQ(ASSERT_RESULT(conn.FetchRow<int32_t>("SELECT * FROM t")), 1);
  }
}

TEST_F(PgSingleTServerTest, BoundedRangeScanWithLargeTransaction) {
  auto conn = ASSERT_RESULT(Connect());
  ASSERT_OK(conn.Execute("CREATE TABLE t (r1 INT, r2 INT, PRIMARY KEY (r1 ASC, r2 ASC))"));
  TestThreadHolder holder;
  for (int i = 0; i != 10; ++i) {
    holder.AddThreadFunctor([this, &stop = holder.stop_flag()] {
      auto conn = ASSERT_RESULT(Connect());
      while (!stop) {
        auto values = ASSERT_RESULT(conn.FetchRows<int32_t>("SELECT r2 FROM t WHERE r2 <= 1"));
        if (!values.empty()) {
          ASSERT_EQ(values, decltype(values){1});
        }
      }
    });
  }
  ASSERT_OK(conn.Execute("INSERT INTO t (SELECT 1, i FROM GENERATE_SERIES(1, 100000) AS i)"));
  holder.WaitAndStop(std::chrono::seconds(5));
}

YB_DEFINE_ENUM(BoundType, (kNone)(kExclusive)(kInclusive));
YB_DEFINE_ENUM(State, (kBefore)(kBetween)(kAfter));

TEST_F(PgSingleTServerTest, Bounds) {
  constexpr int kNumColumns = 3;
  using Row = std::array<int, kNumColumns>;
  constexpr int kNumValues = 6;

  const int kNumBounds = 1 + pow(kElementsInBoundType, 2);
  const int kNumRows = pow(kNumValues, kNumColumns);
  const int kNumCombinations = pow(kNumBounds, kNumColumns);

  auto conn = ASSERT_RESULT(Connect());
  std::vector<Row> rows;
  {
    std::string cmd = "CREATE TABLE t (";
    std::string suffix;
    for (int i = 0; i != kNumColumns; ++i) {
      cmd += Format("r$0 INT, ", i);
      if (i) {
        suffix += ", ";
      }
      suffix += Format("r$0 ASC", i);
    }
    cmd += "PRIMARY KEY (" + suffix + "))";
    ASSERT_OK(conn.Execute(cmd));
  }
  rows.resize(kNumRows);
  for (int i = 0; i != kNumRows; ++i) {
    std::string cmd = "INSERT INTO t VALUES (";
    auto v = i;
    auto div = kNumRows;
    for (int c = 0; c != kNumColumns; ++c) {
      if (c) {
        cmd += ", ";
      }
      div /= kNumValues;
      auto value = v / div;
      cmd += std::to_string(value);
      rows[i][c] = value;
      v %= div;
    }
    cmd += ")";
    ASSERT_OK(conn.Execute(cmd));
  }

  for (int i = 0; i != kNumCombinations; ++i) {
    std::vector<std::function<bool(const Row&)>> conditions;
    std::vector<std::string> conditions_str;
    auto v = i;
    for (int c = 0; c != kNumColumns; ++c) {
      auto bounds = v % kNumBounds;
      v /= kNumBounds;
      if (!bounds) {
        conditions.push_back([c](const Row& row) { return row[c] == 2; });
        conditions_str.push_back(Format("r$0 = 2", c));
        continue;
      }
      --bounds;
      auto lower_bound = static_cast<BoundType>(bounds % kElementsInBoundType);
      auto upper_bound = static_cast<BoundType>(bounds / kElementsInBoundType);
      switch (lower_bound) {
        case BoundType::kNone:
          break;
        case BoundType::kExclusive:
          conditions.push_back([c](const Row& row) { return row[c] > 1; });
          conditions_str.push_back(Format("r$0 > 1", c));
          break;
        case BoundType::kInclusive:
          conditions.push_back([c](const Row& row) { return row[c] >= 1; });
          conditions_str.push_back(Format("r$0 >= 1", c));
          break;
      }
      switch (upper_bound) {
        case BoundType::kNone:
          break;
        case BoundType::kExclusive:
          conditions.push_back([c](const Row& row) { return row[c] < 4; });
          conditions_str.push_back(Format("r$0 < 4", c));
          break;
        case BoundType::kInclusive:
          conditions.push_back([c](const Row& row) { return row[c] <= 4; });
          conditions_str.push_back(Format("r$0 <= 4", c));
          break;
      }
    }
    std::string cmd = "SELECT * FROM t";
    auto initial_length = cmd.length();
    for (const auto& condition : conditions_str) {
      cmd += cmd.length() == initial_length ? " WHERE " : " AND ";
      cmd += condition;
    }
    std::vector<std::tuple<int32_t, int32_t, int32_t>> expected;
    auto state = State::kBefore;
    auto trivial = true;
    for (const auto& row : rows) {
      bool match = true;
      for (const auto& condition : conditions) {
        match = match && condition(row);
      }
      switch (state) {
        case State::kBefore:
          if (match) {
            state = State::kBetween;
          }
          break;
        case State::kBetween:
          if (!match) {
            state = State::kAfter;
          }
          break;
        case State::kAfter:
          if (match) {
            trivial = false;
          }
          break;
      }
      if (match) {
        expected.push_back(std::tuple_cat(row));
      }
    }
    LOG(INFO) << "Trivial: " << trivial << ", cmd: " << cmd;
    ANNOTATE_UNPROTECTED_WRITE(TEST_scan_trivial_expectation) = trivial;
    auto fetched = ASSERT_RESULT((conn.FetchRows<int32_t, int32_t, int32_t>(cmd)));
    ANNOTATE_UNPROTECTED_WRITE(TEST_scan_trivial_expectation) = -1;
    ASSERT_EQ(expected, fetched);
  }
}

TEST_F(PgSingleTServerTest, RangeConflict) {
  auto conn = ASSERT_RESULT(Connect());
  ASSERT_OK(conn.Execute("CREATE TABLE t (key INT, value INT, PRIMARY KEY (key ASC))"));

  auto conn2 = ASSERT_RESULT(Connect());
  ASSERT_OK(conn2.StartTransaction(IsolationLevel::SNAPSHOT_ISOLATION));
  LOG(INFO) << "Rows: " << AsString(conn2.FetchRows<std::string>("SELECT * FROM t"));

  ASSERT_OK(conn.ExecuteFormat("INSERT INTO t VALUES (1, 1), (2, 1)"));
  ASSERT_OK(WaitForAllIntentsApplied(cluster_.get()));
  ASSERT_OK(cluster_->FlushTablets());

  ASSERT_NOK(conn2.Execute("INSERT INTO t VALUES (0, 2), (1, 2)"));
}

TEST_F(PgSingleTServerTest, UpdateIndexWithHole) {
  auto conn = ASSERT_RESULT(Connect());
  ASSERT_OK(conn.Execute("CREATE TABLE t (id INT PRIMARY KEY, value INT)"));
  // Need missing entry in index table, so UPSERT will be the first operation.
  ASSERT_OK(conn.Execute("CREATE INDEX value_idx ON t (value ASC) where value != 2"));
  ASSERT_OK(conn.Execute("INSERT INTO t VALUES (1, 2), (2, 4)"));
  ASSERT_OK(conn.Execute("UPDATE t SET value = value - 1"));

  auto num_index_rows = ASSERT_RESULT(conn.FetchRow<int64_t>(
      "SELECT COUNT(*) FROM t WHERE value > 2"));
  ASSERT_EQ(num_index_rows, 1);
}

}  // namespace pgwrapper
}  // namespace yb
