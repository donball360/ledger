// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "apps/ledger/src/cloud_sync/impl/ledger_sync_impl.h"

#include "apps/ledger/src/network/fake_network_service.h"
#include "apps/ledger/src/test/capture.h"
#include "apps/ledger/src/test/test_with_message_loop.h"
#include "gtest/gtest.h"
#include "lib/ftl/macros.h"
#include "lib/mtl/tasks/message_loop.h"

namespace cloud_sync {

class LedgerSyncImplTest : public test::TestWithMessageLoop {
 public:
  LedgerSyncImplTest()
      : network_service_(mtl::MessageLoop::GetCurrent()->task_runner()),
        environment_(configuration::Configuration(),
                     nullptr,
                     &network_service_),
        ledger_sync_(&environment_, "test_id") {}

  // ::testing::Test:
  void SetUp() override { ::testing::Test::SetUp(); }

  void TearDown() override { ::testing::Test::TearDown(); }

 protected:
  ledger::FakeNetworkService network_service_;
  ledger::Environment environment_;
  LedgerSyncImpl ledger_sync_;

 private:
  FTL_DISALLOW_COPY_AND_ASSIGN(LedgerSyncImplTest);
};

TEST_F(LedgerSyncImplTest, RemoteContainsRequestUrl) {
  network_service_.SetStringResponse("null", 200);
  RemoteResponse response;
  ledger_sync_.RemoteContains(
      "page_id",
      test::Capture([this] { message_loop_.PostQuitTask(); }, &response));
  RunLoopWithTimeout();
  EXPECT_EQ("https://.firebaseio.com/V/1/test_idV/page_idV.json?shallow=true",
            network_service_.GetRequest()->url);
}

TEST_F(LedgerSyncImplTest, RemoteContainsWhenAnswerIsYes) {
  network_service_.SetStringResponse("{\"commits\":true,\"objects\":true}",
                                     200);
  RemoteResponse response;
  ledger_sync_.RemoteContains(
      "page_id",
      test::Capture([this] { message_loop_.PostQuitTask(); }, &response));
  RunLoopWithTimeout();

  EXPECT_EQ(RemoteResponse::FOUND, response);
}

TEST_F(LedgerSyncImplTest, RemoteContainsWhenAnswerIsNo) {
  network_service_.SetStringResponse("null", 200);
  RemoteResponse response;
  ledger_sync_.RemoteContains(
      "page_id",
      test::Capture([this] { message_loop_.PostQuitTask(); }, &response));
  RunLoopWithTimeout();

  EXPECT_EQ(RemoteResponse::NOT_FOUND, response);
}

TEST_F(LedgerSyncImplTest, RemoteContainsWhenServerReturnsError) {
  network_service_.SetStringResponse("null", 500);
  RemoteResponse response;
  ledger_sync_.RemoteContains(
      "page_id",
      test::Capture([this] { message_loop_.PostQuitTask(); }, &response));
  RunLoopWithTimeout();

  EXPECT_EQ(RemoteResponse::SERVER_ERROR, response);
}

}  // namespace cloud_sync
