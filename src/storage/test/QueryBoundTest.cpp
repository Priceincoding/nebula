/* Copyright (c) 2018 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License,
 * attached with Common Clause Condition 1.0, found in the LICENSES directory.
 */

#include "base/Base.h"
#include "base/NebulaKeyUtils.h"
#include <gtest/gtest.h>
#include <rocksdb/db.h>
#include <limits>
#include "fs/TempDir.h"
#include "storage/test/TestUtils.h"
#include "storage/QueryBoundProcessor.h"
#include "dataman/RowSetReader.h"
#include "dataman/RowReader.h"

DECLARE_int32(max_handlers_per_req);
DECLARE_int32(min_vertices_per_bucket);

namespace nebula {
namespace storage {

void mockData(kvstore::KVStore* kv) {
    for (auto partId = 0; partId < 3; partId++) {
        std::vector<kvstore::KV> data;
        for (auto vertexId = partId * 10; vertexId < (partId + 1) * 10; vertexId++) {
            for (auto tagId = 3001; tagId < 3010; tagId++) {
                auto key = NebulaKeyUtils::vertexKey(partId, vertexId, tagId, 0);
                RowWriter writer;
                for (uint64_t numInt = 0; numInt < 3; numInt++) {
                    writer << (vertexId + tagId + numInt);
                }
                for (auto numString = 3; numString < 6; numString++) {
                    writer << folly::stringPrintf("tag_string_col_%d", numString);
                }
                auto val = writer.encode();
                data.emplace_back(std::move(key), std::move(val));
            }
            // Generate 7 out-edges for each edgeType.
            for (auto dstId = 10001; dstId <= 10007; dstId++) {
                VLOG(3) << "Write part " << partId << ", vertex " << vertexId << ", dst " << dstId;
                // Write multi versions,  we should get the latest version.
                for (auto version = 0; version < 3; version++) {
                    auto key = NebulaKeyUtils::edgeKey(partId, vertexId, 101,
                                                       0, dstId,
                                                       std::numeric_limits<int>::max() - version);
                    RowWriter writer(nullptr);
                    for (uint64_t numInt = 0; numInt < 10; numInt++) {
                        writer << (dstId + numInt);
                    }
                    for (auto numString = 10; numString < 20; numString++) {
                        writer << folly::stringPrintf("string_col_%d_%d", numString, version);
                    }
                    auto val = writer.encode();
                    data.emplace_back(std::move(key), std::move(val));
                }
            }
            // Generate 5 in-edges for each edgeType, the edgeType is negative
            for (auto srcId = 20001; srcId <= 20005; srcId++) {
                VLOG(3) << "Write part " << partId << ", vertex " << vertexId << ", src " << srcId;
                for (auto version = 0; version < 3; version++) {
                    auto key = NebulaKeyUtils::edgeKey(partId, vertexId, -101,
                                                       0, srcId,
                                                       std::numeric_limits<int>::max() - version);
                    data.emplace_back(std::move(key), "");
                }
            }
        }
        folly::Baton<true, std::atomic> baton;
        kv->asyncMultiPut(
            0, partId, std::move(data),
            [&](kvstore::ResultCode code) {
                EXPECT_EQ(code, kvstore::ResultCode::SUCCEEDED);
                baton.post();
            });
        baton.wait();
    }
}


void buildRequest(cpp2::GetNeighborsRequest& req, bool outBound = true) {
    req.set_space_id(0);
    decltype(req.parts) tmpIds;
    for (auto partId = 0; partId < 3; partId++) {
        for (auto vertexId =  partId * 10; vertexId < (partId + 1) * 10; vertexId++) {
            tmpIds[partId].emplace_back(vertexId);
        }
    }
    req.set_parts(std::move(tmpIds));
    req.set_edge_type(outBound ? 101 : -101);
    // Return tag props col_0, col_2, col_4
    decltype(req.return_columns) tmpColumns;
    for (int i = 0; i < 3; i++) {
        tmpColumns.emplace_back(
            TestUtils::propDef(cpp2::PropOwner::SOURCE,
                               folly::stringPrintf("tag_%d_col_%d", 3001 + i*2, i*2),
                               3001 + i*2));
    }
    tmpColumns.emplace_back(TestUtils::propDef(cpp2::PropOwner::EDGE, "_dst"));
    tmpColumns.emplace_back(TestUtils::propDef(cpp2::PropOwner::EDGE, "_rank"));
    // Return edge props col_0, col_2, col_4 ... col_18
    for (int i = 0; i < 10; i++) {
        tmpColumns.emplace_back(
            TestUtils::propDef(cpp2::PropOwner::EDGE,
                               folly::stringPrintf("col_%d", i*2)));
    }
    req.set_return_columns(std::move(tmpColumns));
}

void checkResponse(cpp2::QueryResponse& resp,
                   int32_t vertexNum,
                   int32_t edgeFields,
                   int32_t dstIdFrom,
                   int32_t edgeNum,
                   bool outBound) {
    EXPECT_EQ(0, resp.result.failed_codes.size());

    EXPECT_EQ(edgeFields, resp.edge_schema.columns.size());
    EXPECT_EQ(3, resp.vertex_schema.columns.size());
    auto provider = std::make_shared<ResultSchemaProvider>(resp.edge_schema);
    auto tagProvider = std::make_shared<ResultSchemaProvider>(resp.vertex_schema);
    EXPECT_EQ(vertexNum, resp.vertices.size());
    for (auto& vp : resp.vertices) {
        VLOG(1) << "Check vertex props...";
        auto tagReader = RowReader::getRowReader(vp.vertex_data, tagProvider);
        EXPECT_EQ(3, tagReader->numFields());
        int64_t col1;
        EXPECT_EQ(ResultType::SUCCEEDED, tagReader->getInt<int64_t>("tag_3001_col_0", col1));
        EXPECT_EQ(col1, vp.vertex_id + 3001);

        int64_t col2;
        EXPECT_EQ(ResultType::SUCCEEDED, tagReader->getInt<int64_t>("tag_3003_col_2", col2));
        EXPECT_EQ(col2, vp.vertex_id + 3003 + 2);

        folly::StringPiece col3;
        EXPECT_EQ(ResultType::SUCCEEDED, tagReader->getString("tag_3005_col_4", col3));
        EXPECT_EQ(folly::stringPrintf("tag_string_col_4"), col3);

        VLOG(1) << "Check edge props...";
        RowSetReader rsReader(provider, vp.edge_data);
        auto it = rsReader.begin();
        int32_t rowNum = 0;
        while (static_cast<bool>(it)) {
            EXPECT_EQ(edgeFields, it->numFields());
            int64_t dstId;
            {
                // _dst
                EXPECT_EQ(ResultType::SUCCEEDED, it->getInt<int64_t>(0, dstId));
                CHECK_EQ(dstIdFrom + rowNum, dstId);
            }
            {
                // _rank
                int64_t v;
                EXPECT_EQ(ResultType::SUCCEEDED, it->getInt<int64_t>(1, v));
                CHECK_EQ(0, v);
            }
            if (outBound) {
                // col_0, col_2 ... col_8
                for (auto i = 2; i < 7; i++) {
                    int64_t v;
                    EXPECT_EQ(ResultType::SUCCEEDED, it->getInt<int64_t>(i, v));
                    CHECK_EQ((i - 2) * 2 + dstId, v);
                }
                // col_10, col_12 ... col_18
                for (auto i = 7; i < 12; i++) {
                    folly::StringPiece v;
                    EXPECT_EQ(ResultType::SUCCEEDED, it->getString(i, v));
                    CHECK_EQ(folly::stringPrintf("string_col_%d_%d", (i - 7 + 5) * 2, 2), v);
                }
            }
            ++it;
            rowNum++;
        }
        EXPECT_EQ(it, rsReader.end());
        EXPECT_EQ(edgeNum, rowNum);
    }
}


TEST(QueryBoundTest, OutBoundSimpleTest) {
    fs::TempDir rootPath("/tmp/QueryBoundTest.XXXXXX");
    std::unique_ptr<kvstore::KVStore> kv = TestUtils::initKV(rootPath.path());

    LOG(INFO) << "Prepare meta...";
    auto schemaMan = TestUtils::mockSchemaMan();
    mockData(kv.get());

    cpp2::GetNeighborsRequest req;
    buildRequest(req);

    LOG(INFO) << "Test QueryOutBoundRequest...";
    auto executor = std::make_unique<folly::CPUThreadPoolExecutor>(3);
    auto* processor = QueryBoundProcessor::instance(kv.get(), schemaMan.get(), executor.get());
    auto f = processor->getFuture();
    processor->process(req);
    auto resp = std::move(f).get();

    LOG(INFO) << "Check the results...";
    checkResponse(resp, 30, 12, 10001, 7, true);
}


TEST(QueryBoundTest, inBoundSimpleTest) {
    fs::TempDir rootPath("/tmp/QueryBoundTest.XXXXXX");
    LOG(INFO) << "Prepare meta...";
    std::unique_ptr<kvstore::KVStore> kv = TestUtils::initKV(rootPath.path());

    auto schemaMan = TestUtils::mockSchemaMan();
    mockData(kv.get());

    cpp2::GetNeighborsRequest req;
    buildRequest(req, false);

    LOG(INFO) << "Test QueryInBoundRequest...";
    auto executor = std::make_unique<folly::CPUThreadPoolExecutor>(3);
    auto* processor = QueryBoundProcessor::instance(kv.get(), schemaMan.get(),
                                                    executor.get(), BoundType::IN_BOUND);
    auto f = processor->getFuture();
    processor->process(req);
    auto resp = std::move(f).get();

    LOG(INFO) << "Check the results...";
    checkResponse(resp, 30, 2, 20001, 5, false);
}

TEST(QueryBoundTest, FilterTest_OnlyEdgeFilter) {
    fs::TempDir rootPath("/tmp/QueryBoundTest.XXXXXX");
    LOG(INFO) << "Prepare meta...";
    std::unique_ptr<kvstore::KVStore> kv(TestUtils::initKV(rootPath.path()));
    auto schemaMan = TestUtils::mockSchemaMan();
    mockData(kv.get());

    LOG(INFO) << "Build filter...";
    auto* edgeProp = new std::string("col_0");
    auto* alias = new std::string("e101");
    auto* edgeExp = new AliasPropertyExpression(new std::string(""), alias, edgeProp);
    auto* priExp = new PrimaryExpression(10007L);
    auto relExp = std::make_unique<RelationalExpression>(edgeExp,
                                                         RelationalExpression::Operator::GE,
                                                         priExp);
    cpp2::GetNeighborsRequest req;
    buildRequest(req);
    req.set_filter(Expression::encode(relExp.get()));

    LOG(INFO) << "Test QueryOutBoundRequest...";
    auto executor = std::make_unique<folly::CPUThreadPoolExecutor>(3);
    auto* processor = QueryBoundProcessor::instance(kv.get(),
                                                    schemaMan.get(),
                                                    executor.get(),
                                                    BoundType::OUT_BOUND);
    auto f = processor->getFuture();
    processor->process(req);
    auto resp = std::move(f).get();

    LOG(INFO) << "Check the results...";
    checkResponse(resp, 30, 12, 10007, 1, true);
}

TEST(QueryBoundTest, FilterTest_OnlyTagFilter) {
    fs::TempDir rootPath("/tmp/QueryBoundTest.XXXXXX");
    LOG(INFO) << "Prepare meta...";
    std::unique_ptr<kvstore::KVStore> kv(TestUtils::initKV(rootPath.path()));
    auto schemaMan = TestUtils::mockSchemaMan();
    mockData(kv.get());

    LOG(INFO) << "Build filter...";
    auto* tag = new std::string("3001");
    auto* prop = new std::string("tag_3001_col_0");
    auto* srcExp = new SourcePropertyExpression(tag, prop);
    auto* priExp = new PrimaryExpression(20 + 3001L);
    auto relExp = std::make_unique<RelationalExpression>(srcExp,
                                                         RelationalExpression::Operator::GE,
                                                         priExp);
    cpp2::GetNeighborsRequest req;
    buildRequest(req);
    req.set_filter(Expression::encode(relExp.get()));

    LOG(INFO) << "Test QueryOutBoundRequest...";
    auto executor = std::make_unique<folly::CPUThreadPoolExecutor>(3);
    auto* processor = QueryBoundProcessor::instance(kv.get(),
                                                    schemaMan.get(),
                                                    executor.get(),
                                                    BoundType::OUT_BOUND);
    auto f = processor->getFuture();
    processor->process(req);
    auto resp = std::move(f).get();

    LOG(INFO) << "Check the results...";
    checkResponse(resp, 10, 12, 10001, 7, true);
}

TEST(QueryBoundTest,  GenBucketsTest) {
    {
        cpp2::GetNeighborsRequest req;
        buildRequest(req, false);
        QueryBoundProcessor pro(nullptr, nullptr, nullptr, BoundType::OUT_BOUND);
        auto buckets = pro.genBuckets(req);
        ASSERT_EQ(10, buckets.size());
        for (auto& bucket : buckets) {
            ASSERT_EQ(3, bucket.vertices_.size());
        }
    }
    {
        FLAGS_max_handlers_per_req = 9;
        FLAGS_min_vertices_per_bucket = 3;
        cpp2::GetNeighborsRequest req;
        buildRequest(req, false);
        QueryBoundProcessor pro(nullptr, nullptr, nullptr, BoundType::OUT_BOUND);
        auto buckets = pro.genBuckets(req);
        ASSERT_EQ(9, buckets.size());
        for (auto i = 0; i < 3; i++) {
            ASSERT_EQ(4, buckets[i].vertices_.size());
        }
        for (auto i = 3; i < 9; i++) {
            ASSERT_EQ(3, buckets[i].vertices_.size());
        }
    }
    {
        FLAGS_max_handlers_per_req = 40;
        FLAGS_min_vertices_per_bucket = 4;
        cpp2::GetNeighborsRequest req;
        buildRequest(req, false);
        QueryBoundProcessor pro(nullptr, nullptr, nullptr, BoundType::OUT_BOUND);
        auto buckets = pro.genBuckets(req);
        ASSERT_EQ(7, buckets.size());
        for (auto i = 0; i < 2; i++) {
            ASSERT_EQ(5, buckets[i].vertices_.size());
        }
        for (auto i = 2; i < 7; i++) {
            ASSERT_EQ(4, buckets[i].vertices_.size());
        }
    }
    {
        FLAGS_min_vertices_per_bucket = 40;
        cpp2::GetNeighborsRequest req;
        buildRequest(req, false);
        QueryBoundProcessor pro(nullptr, nullptr, nullptr, BoundType::OUT_BOUND);
        auto buckets = pro.genBuckets(req);
        ASSERT_EQ(1, buckets.size());
        ASSERT_EQ(30, buckets[0].vertices_.size());
    }
}

TEST(QueryBoundTest, FilterTest_TagAndEdgeFilter) {
    fs::TempDir rootPath("/tmp/QueryBoundTest.XXXXXX");
    LOG(INFO) << "Prepare meta...";
    std::unique_ptr<kvstore::KVStore> kv(TestUtils::initKV(rootPath.path()));
    auto schemaMan = TestUtils::mockSchemaMan();
    mockData(kv.get());

    LOG(INFO) << "Build filter...";
    auto* tag = new std::string("3001");
    auto* prop = new std::string("tag_3001_col_0");
    auto* srcExp = new SourcePropertyExpression(tag, prop);
    auto* priExp = new PrimaryExpression(20 + 3001L);
    auto* left = new RelationalExpression(srcExp,
                                          RelationalExpression::Operator::GE,
                                          priExp);
    auto* edgeProp = new std::string("col_0");
    auto* alias = new std::string("e101");
    auto* edgeExp = new AliasPropertyExpression(new std::string(""), alias, edgeProp);
    auto* priExp2 = new PrimaryExpression(10007L);
    auto* right = new RelationalExpression(edgeExp,
                                           RelationalExpression::Operator::GE,
                                           priExp2);
    auto logExp = std::make_unique<LogicalExpression>(left, LogicalExpression::AND, right);

    cpp2::GetNeighborsRequest req;
    buildRequest(req);
    req.set_filter(Expression::encode(logExp.get()));

    LOG(INFO) << "Test QueryOutBoundRequest...";
    auto executor = std::make_unique<folly::CPUThreadPoolExecutor>(3);
    auto* processor = QueryBoundProcessor::instance(kv.get(),
                                                    schemaMan.get(),
                                                    executor.get(),
                                                    BoundType::OUT_BOUND);
    auto f = processor->getFuture();
    processor->process(req);
    auto resp = std::move(f).get();

    LOG(INFO) << "Check the results...";
    checkResponse(resp, 10, 12, 10007, 1, true);
}

TEST(QueryBoundTest, FilterTest_InvalidFilter) {
    fs::TempDir rootPath("/tmp/QueryBoundTest.XXXXXX");
    LOG(INFO) << "Prepare meta...";
    std::unique_ptr<kvstore::KVStore> kv(TestUtils::initKV(rootPath.path()));
    auto schemaMan = TestUtils::mockSchemaMan();
    mockData(kv.get());

    LOG(INFO) << "Build filter...";
    auto* prop = new std::string("tag_3001_col_0");
    auto inputExp = std::make_unique<InputPropertyExpression>(prop);

    cpp2::GetNeighborsRequest req;
    buildRequest(req);
    req.set_filter(Expression::encode(inputExp.get()));

    LOG(INFO) << "Test QueryOutBoundRequest...";
    auto executor = std::make_unique<folly::CPUThreadPoolExecutor>(3);
    auto* processor = QueryBoundProcessor::instance(kv.get(),
                                                    schemaMan.get(),
                                                    executor.get(),
                                                    BoundType::OUT_BOUND);
    auto f = processor->getFuture();
    processor->process(req);
    auto resp = std::move(f).get();

    LOG(INFO) << "Check the results...";
    EXPECT_EQ(3, resp.result.failed_codes.size());
    EXPECT_TRUE(nebula::storage::cpp2::ErrorCode::E_INVALID_FILTER
                    == resp.result.failed_codes[0].code);
}
}  // namespace storage
}  // namespace nebula


int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    folly::init(&argc, &argv, true);
    google::SetStderrLogging(google::INFO);
    return RUN_ALL_TESTS();
}


