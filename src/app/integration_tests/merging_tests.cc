// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <utility>
#include <vector>

#include "apps/ledger/services/public/ledger.fidl.h"
#include "apps/ledger/src/app/integration_tests/test_utils.h"
#include "apps/ledger/src/convert/convert.h"
#include "gtest/gtest.h"
#include "lib/fidl/cpp/bindings/binding.h"
#include "lib/ftl/functional/make_copyable.h"
#include "lib/ftl/macros.h"
#include "lib/mtl/tasks/message_loop.h"

namespace ledger {
namespace integration_tests {
namespace {

class MergingIntegrationTest : public LedgerApplicationBaseTest {
 public:
  MergingIntegrationTest() {}
  ~MergingIntegrationTest() override {}

 private:
  FTL_DISALLOW_COPY_AND_ASSIGN(MergingIntegrationTest);
};

class Watcher : public PageWatcher {
 public:
  Watcher(fidl::InterfaceRequest<PageWatcher> request,
          ftl::Closure change_callback)
      : binding_(this, std::move(request)), change_callback_(change_callback) {}

  uint changes_seen = 0;
  PageSnapshotPtr last_snapshot_;
  PageChangePtr last_page_change_;

 private:
  // PageWatcher:
  void OnChange(PageChangePtr page_change,
                ResultState result_state,
                const OnChangeCallback& callback) override {
    FTL_DCHECK(page_change);
    FTL_DCHECK(result_state == ResultState::COMPLETED)
        << "Handling OnChange pagination not implemented yet";
    changes_seen++;
    last_page_change_ = std::move(page_change);
    last_snapshot_.reset();
    callback(last_snapshot_.NewRequest());
    change_callback_();
  }

  fidl::Binding<PageWatcher> binding_;
  ftl::Closure change_callback_;
};

class ConflictResolverImpl : public ConflictResolver {
 public:
  explicit ConflictResolverImpl(
      fidl::InterfaceRequest<ConflictResolver> request)
      : binding_(this, std::move(request)) {
    binding_.set_connection_error_handler(
        [this] { this->disconnected = true; });
  }
  ~ConflictResolverImpl() {}

  struct ResolveRequest {
    fidl::InterfaceHandle<PageSnapshot> left_version;
    PageChangePtr change_left;
    fidl::InterfaceHandle<PageSnapshot> right_version;
    PageChangePtr change_right;
    fidl::InterfaceHandle<PageSnapshot> common_version;
    ResolveCallback callback;

    ResolveRequest(fidl::InterfaceHandle<PageSnapshot> left_version,
                   PageChangePtr change_left,
                   fidl::InterfaceHandle<PageSnapshot> right_version,
                   PageChangePtr change_right,
                   fidl::InterfaceHandle<PageSnapshot> common_version,
                   const ResolveCallback& callback)
        : left_version(std::move(left_version)),
          change_left(std::move(change_left)),
          right_version(std::move(right_version)),
          change_right(std::move(change_right)),
          common_version(std::move(common_version)),
          callback(callback) {}
  };

  std::vector<ResolveRequest> requests;
  bool disconnected = false;

 private:
  // ConflictResolver:
  void Resolve(fidl::InterfaceHandle<PageSnapshot> left_version,
               PageChangePtr change_left,
               fidl::InterfaceHandle<PageSnapshot> right_version,
               PageChangePtr change_right,
               fidl::InterfaceHandle<PageSnapshot> common_version,
               const ResolveCallback& callback) override {
    requests.emplace_back(std::move(left_version), std::move(change_left),
                          std::move(right_version), std::move(change_right),
                          std::move(common_version), callback);
    mtl::MessageLoop::GetCurrent()->PostQuitTask();
  }

  fidl::Binding<ConflictResolver> binding_;
};

class TestConflictResolverFactory : public ConflictResolverFactory {
 public:
  TestConflictResolverFactory(
      MergePolicy policy,
      fidl::InterfaceRequest<ConflictResolverFactory> request,
      ftl::Closure on_get_policy_called_callback)
      : policy_(policy),
        binding_(this, std::move(request)),
        callback_(on_get_policy_called_callback) {}

  uint get_policy_calls = 0;
  std::unordered_map<storage::PageId, ConflictResolverImpl> resolvers;

 private:
  // ConflictResolverFactory:
  void GetPolicy(fidl::Array<uint8_t> page_id,
                 const GetPolicyCallback& callback) override {
    get_policy_calls++;
    callback(policy_);
    if (callback_) {
      callback_();
    }
  }

  void NewConflictResolver(
      fidl::Array<uint8_t> page_id,
      fidl::InterfaceRequest<ConflictResolver> resolver) override {
    resolvers.emplace(std::piecewise_construct,
                      std::forward_as_tuple(convert::ToString(page_id)),
                      std::forward_as_tuple(std::move(resolver)));
  }

  MergePolicy policy_;
  fidl::Binding<ConflictResolverFactory> binding_;
  ftl::Closure callback_;
};

TEST_F(MergingIntegrationTest, Merging) {
  PagePtr page1 = GetTestPage();
  fidl::Array<uint8_t> test_page_id;
  page1->GetId([&test_page_id](fidl::Array<uint8_t> page_id) {
    test_page_id = std::move(page_id);
  });
  EXPECT_TRUE(page1.WaitForIncomingResponse());

  PagePtr page2 = GetPage(test_page_id, Status::OK);

  PageWatcherPtr watcher1_ptr;
  Watcher watcher1(GetProxy(&watcher1_ptr),
                   [] { mtl::MessageLoop::GetCurrent()->PostQuitTask(); });
  PageSnapshotPtr snapshot1;
  page1->GetSnapshot(snapshot1.NewRequest(), std::move(watcher1_ptr),
                     [](Status status) { EXPECT_EQ(Status::OK, status); });
  EXPECT_TRUE(page1.WaitForIncomingResponse());

  PageWatcherPtr watcher2_ptr;
  Watcher watcher2(GetProxy(&watcher2_ptr),
                   [] { mtl::MessageLoop::GetCurrent()->PostQuitTask(); });
  PageSnapshotPtr snapshot2;
  page2->GetSnapshot(snapshot2.NewRequest(), std::move(watcher2_ptr),
                     [](Status status) { EXPECT_EQ(Status::OK, status); });
  EXPECT_TRUE(page2.WaitForIncomingResponse());

  page1->StartTransaction([](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(page1.WaitForIncomingResponse());
  page1->Put(convert::ToArray("name"), convert::ToArray("Alice"),
             [](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(page1.WaitForIncomingResponse());
  page1->Put(convert::ToArray("city"), convert::ToArray("Paris"),
             [](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(page1.WaitForIncomingResponse());

  page2->StartTransaction([](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(page2.WaitForIncomingResponse());
  page2->Put(convert::ToArray("name"), convert::ToArray("Bob"),
             [](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(page2.WaitForIncomingResponse());
  page2->Put(convert::ToArray("phone"), convert::ToArray("0123456789"),
             [](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(page2.WaitForIncomingResponse());

  // Verify that each change is seen by the right watcher.
  page1->Commit([](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(page1.WaitForIncomingResponse());
  ASSERT_FALSE(RunLoopWithTimeout());
  EXPECT_EQ(1u, watcher1.changes_seen);
  PageChangePtr change = std::move(watcher1.last_page_change_);
  ASSERT_EQ(2u, change->changes.size());
  EXPECT_EQ("city", convert::ToString(change->changes[0]->key));
  EXPECT_EQ("Paris", ToString(change->changes[0]->value));
  EXPECT_EQ("name", convert::ToString(change->changes[1]->key));
  EXPECT_EQ("Alice", ToString(change->changes[1]->value));

  page2->Commit([](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(page2.WaitForIncomingResponse());
  ASSERT_FALSE(RunLoopWithTimeout());

  EXPECT_EQ(1u, watcher2.changes_seen);
  change = std::move(watcher2.last_page_change_);
  ASSERT_EQ(2u, change->changes.size());
  EXPECT_EQ("name", convert::ToString(change->changes[0]->key));
  EXPECT_EQ("Bob", ToString(change->changes[0]->value));
  EXPECT_EQ("phone", convert::ToString(change->changes[1]->key));
  EXPECT_EQ("0123456789", ToString(change->changes[1]->value));

  ASSERT_FALSE(RunLoopWithTimeout());
  ASSERT_FALSE(RunLoopWithTimeout());
  // Each change is seen once, and by the correct watcher only.
  EXPECT_EQ(2u, watcher1.changes_seen);
  change = std::move(watcher1.last_page_change_);
  ASSERT_EQ(2u, change->changes.size());
  EXPECT_EQ("name", convert::ToString(change->changes[0]->key));
  EXPECT_EQ("Bob", ToString(change->changes[0]->value));
  EXPECT_EQ("phone", convert::ToString(change->changes[1]->key));
  EXPECT_EQ("0123456789", ToString(change->changes[1]->value));

  EXPECT_EQ(2u, watcher2.changes_seen);
  change = std::move(watcher2.last_page_change_);
  ASSERT_EQ(1u, change->changes.size());
  EXPECT_EQ("city", convert::ToString(change->changes[0]->key));
  EXPECT_EQ("Paris", ToString(change->changes[0]->value));
}

TEST_F(MergingIntegrationTest, MergingWithConflictResolutionFactory) {
  PagePtr page1 = GetTestPage();
  fidl::Array<uint8_t> test_page_id;
  page1->GetId([&test_page_id](fidl::Array<uint8_t> page_id) {
    test_page_id = std::move(page_id);
  });
  EXPECT_TRUE(page1.WaitForIncomingResponse());

  // Set up a resolver
  ConflictResolverFactoryPtr resolver_factory_ptr;
  std::unique_ptr<TestConflictResolverFactory> resolver_factory =
      std::make_unique<TestConflictResolverFactory>(
          MergePolicy::NONE, GetProxy(&resolver_factory_ptr),
          [] { mtl::MessageLoop::GetCurrent()->PostQuitTask(); });
  LedgerPtr ledger_ptr = GetTestLedger();
  ledger_ptr->SetConflictResolverFactory(
      std::move(resolver_factory_ptr),
      [](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(ledger_ptr.WaitForIncomingResponse());
  EXPECT_FALSE(RunLoopWithTimeout());

  PagePtr page2 = GetPage(test_page_id, Status::OK);

  PageWatcherPtr watcher1_ptr;
  Watcher watcher1(GetProxy(&watcher1_ptr),
                   [] { mtl::MessageLoop::GetCurrent()->PostQuitTask(); });
  PageSnapshotPtr snapshot1;
  page1->GetSnapshot(snapshot1.NewRequest(), std::move(watcher1_ptr),
                     [](Status status) { EXPECT_EQ(Status::OK, status); });
  EXPECT_TRUE(page1.WaitForIncomingResponse());

  PageWatcherPtr watcher2_ptr;
  Watcher watcher2(GetProxy(&watcher2_ptr),
                   [] { mtl::MessageLoop::GetCurrent()->PostQuitTask(); });
  PageSnapshotPtr snapshot2;
  page2->GetSnapshot(snapshot2.NewRequest(), std::move(watcher2_ptr),
                     [](Status status) { EXPECT_EQ(Status::OK, status); });
  EXPECT_TRUE(page2.WaitForIncomingResponse());

  page1->StartTransaction([](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(page1.WaitForIncomingResponse());
  page1->Put(convert::ToArray("name"), convert::ToArray("Alice"),
             [](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(page1.WaitForIncomingResponse());
  page1->Put(convert::ToArray("city"), convert::ToArray("Paris"),
             [](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(page1.WaitForIncomingResponse());

  page2->StartTransaction([](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(page2.WaitForIncomingResponse());
  page2->Put(convert::ToArray("name"), convert::ToArray("Bob"),
             [](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(page2.WaitForIncomingResponse());
  page2->Put(convert::ToArray("phone"), convert::ToArray("0123456789"),
             [](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(page2.WaitForIncomingResponse());

  // Verify that each change is seen by the right watcher.
  page1->Commit([](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(page1.WaitForIncomingResponse());
  EXPECT_FALSE(RunLoopWithTimeout());

  EXPECT_EQ(1u, watcher1.changes_seen);
  PageChangePtr change = std::move(watcher1.last_page_change_);
  ASSERT_EQ(2u, change->changes.size());
  EXPECT_EQ("city", convert::ToString(change->changes[0]->key));
  EXPECT_EQ("Paris", ToString(change->changes[0]->value));
  EXPECT_EQ("name", convert::ToString(change->changes[1]->key));
  EXPECT_EQ("Alice", ToString(change->changes[1]->value));

  page2->Commit([](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(page2.WaitForIncomingResponse());
  EXPECT_FALSE(RunLoopWithTimeout());

  EXPECT_EQ(1u, watcher2.changes_seen);
  change = std::move(watcher2.last_page_change_);
  ASSERT_EQ(2u, change->changes.size());
  EXPECT_EQ("name", convert::ToString(change->changes[0]->key));
  EXPECT_EQ("Bob", ToString(change->changes[0]->value));
  EXPECT_EQ("phone", convert::ToString(change->changes[1]->key));
  EXPECT_EQ("0123456789", ToString(change->changes[1]->value));
  EXPECT_TRUE(RunLoopWithTimeout());
  EXPECT_EQ(1u, resolver_factory->get_policy_calls);

  // Change the merge strategy.
  resolver_factory_ptr.reset();
  resolver_factory = std::make_unique<TestConflictResolverFactory>(
      MergePolicy::LAST_ONE_WINS, GetProxy(&resolver_factory_ptr),
      [] { mtl::MessageLoop::GetCurrent()->PostQuitTask(); });
  ledger_ptr->SetConflictResolverFactory(
      std::move(resolver_factory_ptr),
      [](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(ledger_ptr.WaitForIncomingResponse());

  EXPECT_FALSE(RunLoopWithTimeout());
  EXPECT_FALSE(RunLoopWithTimeout());
  EXPECT_FALSE(RunLoopWithTimeout());

  // Each change is seen once, and by the correct watcher only.
  EXPECT_EQ(2u, watcher1.changes_seen);
  change = std::move(watcher1.last_page_change_);
  ASSERT_EQ(2u, change->changes.size());
  EXPECT_EQ("name", convert::ToString(change->changes[0]->key));
  EXPECT_EQ("Bob", ToString(change->changes[0]->value));
  EXPECT_EQ("phone", convert::ToString(change->changes[1]->key));
  EXPECT_EQ("0123456789", ToString(change->changes[1]->value));

  EXPECT_EQ(2u, watcher2.changes_seen);
  change = std::move(watcher2.last_page_change_);
  ASSERT_EQ(1u, change->changes.size());
  EXPECT_EQ("city", convert::ToString(change->changes[0]->key));
  EXPECT_EQ("Paris", ToString(change->changes[0]->value));

  EXPECT_EQ(1u, resolver_factory->get_policy_calls);
}

TEST_F(MergingIntegrationTest, CustomConflictResolutionNoConflict) {
  ConflictResolverFactoryPtr resolver_factory_ptr;
  std::unique_ptr<TestConflictResolverFactory> resolver_factory =
      std::make_unique<TestConflictResolverFactory>(
          MergePolicy::CUSTOM, GetProxy(&resolver_factory_ptr), nullptr);
  LedgerPtr ledger_ptr = GetTestLedger();
  ledger_ptr->SetConflictResolverFactory(
      std::move(resolver_factory_ptr),
      [](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(ledger_ptr.WaitForIncomingResponse());

  PagePtr page1 = GetTestPage();
  fidl::Array<uint8_t> test_page_id;
  page1->GetId([&test_page_id](fidl::Array<uint8_t> page_id) {
    test_page_id = std::move(page_id);
  });
  EXPECT_TRUE(page1.WaitForIncomingResponse());
  PagePtr page2 = GetPage(test_page_id, Status::OK);

  page1->StartTransaction([](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(page1.WaitForIncomingResponse());
  page1->Put(convert::ToArray("name"), convert::ToArray("Alice"),
             [](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(page1.WaitForIncomingResponse());
  page1->Put(convert::ToArray("city"), convert::ToArray("Paris"),
             [](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(page1.WaitForIncomingResponse());

  page2->StartTransaction([](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(page2.WaitForIncomingResponse());
  page2->Put(convert::ToArray("phone"), convert::ToArray("0123456789"),
             [](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(page2.WaitForIncomingResponse());
  page2->Put(convert::ToArray("email"), convert::ToArray("alice@example.org"),
             [](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(page2.WaitForIncomingResponse());

  page1->Commit([](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(page1.WaitForIncomingResponse());
  page2->Commit([](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(page2.WaitForIncomingResponse());

  EXPECT_FALSE(RunLoopWithTimeout());

  // We now have a conflict.
  EXPECT_EQ(1u, resolver_factory->resolvers.size());
  EXPECT_NE(resolver_factory->resolvers.end(),
            resolver_factory->resolvers.find(convert::ToString(test_page_id)));
  ConflictResolverImpl* resolver_impl =
      &(resolver_factory->resolvers.find(convert::ToString(test_page_id))
            ->second);
  ASSERT_EQ(1u, resolver_impl->requests.size());

  // Left change is the most recent, so the one made on |page2|.
  ASSERT_EQ(2u, resolver_impl->requests[0].change_left->changes.size());
  EXPECT_EQ("email",
            convert::ExtendedStringView(
                resolver_impl->requests[0].change_left->changes[0]->key));
  EXPECT_EQ(
      "alice@example.org",
      ToString(resolver_impl->requests[0].change_left->changes[0]->value));
  EXPECT_EQ("phone",
            convert::ExtendedStringView(
                resolver_impl->requests[0].change_left->changes[1]->key));
  EXPECT_EQ(
      "0123456789",
      ToString(resolver_impl->requests[0].change_left->changes[1]->value));
  // Right change comes from |page1|.
  ASSERT_EQ(2u, resolver_impl->requests[0].change_right->changes.size());
  EXPECT_EQ("city",
            convert::ExtendedStringView(
                resolver_impl->requests[0].change_right->changes[0]->key));
  EXPECT_EQ(
      "Paris",
      ToString(resolver_impl->requests[0].change_right->changes[0]->value));
  EXPECT_EQ("name",
            convert::ExtendedStringView(
                resolver_impl->requests[0].change_right->changes[1]->key));
  EXPECT_EQ(
      "Alice",
      ToString(resolver_impl->requests[0].change_right->changes[1]->value));
  // Common ancestor is empty.
  PageSnapshotPtr snapshot = PageSnapshotPtr::Create(
      std::move(resolver_impl->requests[0].common_version));
  fidl::Array<EntryPtr> entries =
      SnapshotGetEntries(&snapshot, fidl::Array<uint8_t>());
  EXPECT_EQ(0u, entries.size());

  // Prepare the merged values
  fidl::Array<MergedValuePtr> merged_values =
      fidl::Array<MergedValuePtr>::New(0);
  {
    MergedValuePtr merged_value = MergedValue::New();
    merged_value->key = convert::ToArray("name");
    merged_value->source = ValueSource::RIGHT;
    merged_values.push_back(std::move(merged_value));
  }
  {
    MergedValuePtr merged_value = MergedValue::New();
    merged_value->key = convert::ToArray("email");
    merged_value->source = ValueSource::DELETE;
    merged_values.push_back(std::move(merged_value));
  }
  {
    MergedValuePtr merged_value = MergedValue::New();
    merged_value->key = convert::ToArray("pager");
    merged_value->source = ValueSource::NEW;
    BytesOrReferencePtr value = BytesOrReference::New();
    value->set_bytes(convert::ToArray("pager@example.org"));
    merged_value->new_value = std::move(value);
    merged_values.push_back(std::move(merged_value));
  }

  // Watch for the change.
  PageWatcherPtr watcher_ptr;
  Watcher watcher(GetProxy(&watcher_ptr),
                  [] { mtl::MessageLoop::GetCurrent()->PostQuitTask(); });
  PageSnapshotPtr snapshot2;
  page1->GetSnapshot(snapshot2.NewRequest(), std::move(watcher_ptr),
                     [](Status status) { EXPECT_EQ(Status::OK, status); });
  EXPECT_TRUE(page1.WaitForIncomingResponse());

  resolver_impl->requests[0].callback(std::move(merged_values));

  // Wait for the watcher to be called.
  EXPECT_FALSE(RunLoopWithTimeout());

  fidl::Array<EntryPtr> final_entries =
      SnapshotGetEntries(&watcher.last_snapshot_, fidl::Array<uint8_t>());
  ASSERT_EQ(3u, final_entries.size());
  EXPECT_EQ("name", convert::ExtendedStringView(final_entries[0]->key));
  EXPECT_EQ("pager", convert::ExtendedStringView(final_entries[1]->key));
  EXPECT_EQ("phone", convert::ExtendedStringView(final_entries[2]->key));
}

TEST_F(MergingIntegrationTest, CustomConflictResolutionClosingPipe) {
  ConflictResolverFactoryPtr resolver_factory_ptr;
  std::unique_ptr<TestConflictResolverFactory> resolver_factory =
      std::make_unique<TestConflictResolverFactory>(
          MergePolicy::CUSTOM, GetProxy(&resolver_factory_ptr), nullptr);
  LedgerPtr ledger_ptr = GetTestLedger();
  ledger_ptr->SetConflictResolverFactory(
      std::move(resolver_factory_ptr),
      [](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(ledger_ptr.WaitForIncomingResponse());

  PagePtr page1 = GetTestPage();
  fidl::Array<uint8_t> test_page_id;
  page1->GetId([&test_page_id](fidl::Array<uint8_t> page_id) {
    test_page_id = std::move(page_id);
  });
  EXPECT_TRUE(page1.WaitForIncomingResponse());
  PagePtr page2 = GetPage(test_page_id, Status::OK);

  page1->StartTransaction([](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(page1.WaitForIncomingResponse());
  page1->Put(convert::ToArray("name"), convert::ToArray("Alice"),
             [](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(page1.WaitForIncomingResponse());

  page2->StartTransaction([](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(page2.WaitForIncomingResponse());
  page2->Put(convert::ToArray("name"), convert::ToArray("Bob"),
             [](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(page2.WaitForIncomingResponse());

  page1->Commit([](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(page1.WaitForIncomingResponse());
  page2->Commit([](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(page2.WaitForIncomingResponse());

  EXPECT_FALSE(RunLoopWithTimeout());

  // We now have a conflict.
  EXPECT_EQ(1u, resolver_factory->resolvers.size());
  EXPECT_NE(resolver_factory->resolvers.end(),
            resolver_factory->resolvers.find(convert::ToString(test_page_id)));
  ConflictResolverImpl* resolver_impl =
      &(resolver_factory->resolvers.find(convert::ToString(test_page_id))
            ->second);
  EXPECT_EQ(1u, resolver_impl->requests.size());

  // Kill the resolver
  resolver_factory->resolvers.clear();
  EXPECT_EQ(0u, resolver_factory->resolvers.size());

  EXPECT_FALSE(RunLoopWithTimeout());

  // We should ask again for a resolution.
  EXPECT_EQ(1u, resolver_factory->resolvers.size());
  EXPECT_NE(resolver_factory->resolvers.end(),
            resolver_factory->resolvers.find(convert::ToString(test_page_id)));
  resolver_impl =
      &(resolver_factory->resolvers.find(convert::ToString(test_page_id))
            ->second);
  ASSERT_EQ(1u, resolver_impl->requests.size());

  // Remove all references to a page:
  page1 = nullptr;
  page2 = nullptr;
  EXPECT_TRUE(RunLoopWithTimeout(ftl::TimeDelta::FromMilliseconds(500)));

  // Resolution should not crash the Ledger
  fidl::Array<MergedValuePtr> merged_values =
      fidl::Array<MergedValuePtr>::New(0);
  resolver_impl->requests[0].callback(std::move(merged_values));
  EXPECT_TRUE(RunLoopWithTimeout(ftl::TimeDelta::FromMilliseconds(200)));
}

TEST_F(MergingIntegrationTest, CustomConflictResolutionResetFactory) {
  ConflictResolverFactoryPtr resolver_factory_ptr;
  std::unique_ptr<TestConflictResolverFactory> resolver_factory =
      std::make_unique<TestConflictResolverFactory>(
          MergePolicy::CUSTOM, GetProxy(&resolver_factory_ptr), nullptr);
  LedgerPtr ledger_ptr = GetTestLedger();
  ledger_ptr->SetConflictResolverFactory(
      std::move(resolver_factory_ptr),
      [](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(ledger_ptr.WaitForIncomingResponse());

  PagePtr page1 = GetTestPage();
  fidl::Array<uint8_t> test_page_id;
  page1->GetId([&test_page_id](fidl::Array<uint8_t> page_id) {
    test_page_id = std::move(page_id);
  });
  EXPECT_TRUE(page1.WaitForIncomingResponse());
  PagePtr page2 = GetPage(test_page_id, Status::OK);

  page1->StartTransaction([](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(page1.WaitForIncomingResponse());
  page1->Put(convert::ToArray("name"), convert::ToArray("Alice"),
             [](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(page1.WaitForIncomingResponse());

  page2->StartTransaction([](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(page2.WaitForIncomingResponse());
  page2->Put(convert::ToArray("name"), convert::ToArray("Bob"),
             [](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(page2.WaitForIncomingResponse());

  page1->Commit([](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(page1.WaitForIncomingResponse());
  page2->Commit([](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(page2.WaitForIncomingResponse());

  EXPECT_FALSE(RunLoopWithTimeout());

  // We now have a conflict.
  EXPECT_EQ(1u, resolver_factory->resolvers.size());
  EXPECT_NE(resolver_factory->resolvers.end(),
            resolver_factory->resolvers.find(convert::ToString(test_page_id)));
  ConflictResolverImpl* resolver_impl =
      &(resolver_factory->resolvers.find(convert::ToString(test_page_id))
            ->second);
  EXPECT_FALSE(resolver_impl->disconnected);
  EXPECT_EQ(1u, resolver_impl->requests.size());

  // Change the factory.
  ConflictResolverFactoryPtr resolver_factory_ptr2;
  std::unique_ptr<TestConflictResolverFactory> resolver_factory2 =
      std::make_unique<TestConflictResolverFactory>(
          MergePolicy::CUSTOM, GetProxy(&resolver_factory_ptr2), nullptr);
  ledger_ptr->SetConflictResolverFactory(
      std::move(resolver_factory_ptr2),
      [](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(ledger_ptr.WaitForIncomingResponse());

  EXPECT_FALSE(RunLoopWithTimeout());

  // The previous resolver should have been disconnected.
  EXPECT_TRUE(resolver_impl->disconnected);
  // We should ask again for a resolution.
  EXPECT_EQ(1u, resolver_factory2->resolvers.size());
  EXPECT_NE(resolver_factory2->resolvers.end(),
            resolver_factory2->resolvers.find(convert::ToString(test_page_id)));
  ConflictResolverImpl* resolver_impl2 =
      &(resolver_factory2->resolvers.find(convert::ToString(test_page_id))
            ->second);
  ASSERT_EQ(1u, resolver_impl2->requests.size());

  // Remove all references to a page:
  page1 = nullptr;
  page2 = nullptr;
  EXPECT_TRUE(RunLoopWithTimeout(ftl::TimeDelta::FromMilliseconds(500)));

  // Resolution should not crash the Ledger
  fidl::Array<MergedValuePtr> merged_values =
      fidl::Array<MergedValuePtr>::New(0);
  resolver_impl2->requests[0].callback(std::move(merged_values));
  EXPECT_TRUE(RunLoopWithTimeout(ftl::TimeDelta::FromMilliseconds(200)));
}

TEST_F(MergingIntegrationTest, AutoConflictResolutionNoConflict) {
  ConflictResolverFactoryPtr resolver_factory_ptr;
  std::unique_ptr<TestConflictResolverFactory> resolver_factory =
      std::make_unique<TestConflictResolverFactory>(
          MergePolicy::AUTOMATIC_WITH_FALLBACK, GetProxy(&resolver_factory_ptr),
          nullptr);
  LedgerPtr ledger_ptr = GetTestLedger();
  ledger_ptr->SetConflictResolverFactory(
      std::move(resolver_factory_ptr),
      [](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(ledger_ptr.WaitForIncomingResponse());

  PagePtr page1 = GetTestPage();
  fidl::Array<uint8_t> test_page_id;
  page1->GetId([&test_page_id](fidl::Array<uint8_t> page_id) {
    test_page_id = std::move(page_id);
  });
  EXPECT_TRUE(page1.WaitForIncomingResponse());
  PagePtr page2 = GetPage(test_page_id, Status::OK);

  // Watch for changes.
  PageWatcherPtr watcher_ptr;
  Watcher watcher(GetProxy(&watcher_ptr),
                  []() { mtl::MessageLoop::GetCurrent()->PostQuitTask(); });
  PageSnapshotPtr snapshot2;
  page1->GetSnapshot(snapshot2.NewRequest(), std::move(watcher_ptr),
                     [](Status status) { EXPECT_EQ(Status::OK, status); });
  EXPECT_TRUE(page1.WaitForIncomingResponse());

  page1->StartTransaction([](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(page1.WaitForIncomingResponse());
  page1->Put(convert::ToArray("name"), convert::ToArray("Alice"),
             [](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(page1.WaitForIncomingResponse());
  page1->Put(convert::ToArray("city"), convert::ToArray("Paris"),
             [](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(page1.WaitForIncomingResponse());

  page2->StartTransaction([](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(page2.WaitForIncomingResponse());
  page2->Put(convert::ToArray("email"), convert::ToArray("alice@example.org"),
             [](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(page2.WaitForIncomingResponse());
  page2->Put(convert::ToArray("phone"), convert::ToArray("0123456789"),
             [](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(page2.WaitForIncomingResponse());

  page1->Commit([](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(page1.WaitForIncomingResponse());

  EXPECT_FALSE(RunLoopWithTimeout());
  // We should a have seen the first commit at this point
  EXPECT_EQ(1u, watcher.changes_seen);

  page2->Commit([](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(page2.WaitForIncomingResponse());

  EXPECT_FALSE(RunLoopWithTimeout());

  // We now have an automatically-resolved conflict.
  EXPECT_EQ(1u, resolver_factory->resolvers.size());
  EXPECT_NE(resolver_factory->resolvers.end(),
            resolver_factory->resolvers.find(convert::ToString(test_page_id)));
  ConflictResolverImpl* resolver_impl =
      &(resolver_factory->resolvers.find(convert::ToString(test_page_id))
            ->second);
  // We haven't been asked to resolve anything.
  EXPECT_EQ(0u, resolver_impl->requests.size());

  EXPECT_EQ(2u, watcher.changes_seen);

  fidl::Array<EntryPtr> final_entries =
      SnapshotGetEntries(&watcher.last_snapshot_, fidl::Array<uint8_t>());
  ASSERT_EQ(4u, final_entries.size());
  EXPECT_EQ("city", convert::ExtendedStringView(final_entries[0]->key));
  EXPECT_EQ("email", convert::ExtendedStringView(final_entries[1]->key));
  EXPECT_EQ("name", convert::ExtendedStringView(final_entries[2]->key));
  EXPECT_EQ("phone", convert::ExtendedStringView(final_entries[3]->key));
}

TEST_F(MergingIntegrationTest, AutoConflictResolutionWithConflict) {
  ConflictResolverFactoryPtr resolver_factory_ptr;
  std::unique_ptr<TestConflictResolverFactory> resolver_factory =
      std::make_unique<TestConflictResolverFactory>(
          MergePolicy::AUTOMATIC_WITH_FALLBACK, GetProxy(&resolver_factory_ptr),
          nullptr);
  LedgerPtr ledger_ptr = GetTestLedger();
  ledger_ptr->SetConflictResolverFactory(
      std::move(resolver_factory_ptr),
      [](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(ledger_ptr.WaitForIncomingResponse());

  PagePtr page1 = GetTestPage();
  fidl::Array<uint8_t> test_page_id;
  page1->GetId([&test_page_id](fidl::Array<uint8_t> page_id) {
    test_page_id = std::move(page_id);
  });
  EXPECT_TRUE(page1.WaitForIncomingResponse());
  PagePtr page2 = GetPage(test_page_id, Status::OK);

  page1->StartTransaction([](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(page1.WaitForIncomingResponse());
  page1->Put(convert::ToArray("city"), convert::ToArray("Paris"),
             [](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(page1.WaitForIncomingResponse());

  page2->StartTransaction([](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(page2.WaitForIncomingResponse());
  page2->Put(convert::ToArray("name"), convert::ToArray("Alice"),
             [](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(page2.WaitForIncomingResponse());
  page2->Put(convert::ToArray("city"), convert::ToArray("San Francisco"),
             [](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(page2.WaitForIncomingResponse());

  page1->Commit([](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(page1.WaitForIncomingResponse());
  page2->Commit([](Status status) { EXPECT_EQ(status, Status::OK); });
  EXPECT_TRUE(page2.WaitForIncomingResponse());

  EXPECT_FALSE(RunLoopWithTimeout());

  // We now have a conflict.
  EXPECT_EQ(1u, resolver_factory->resolvers.size());
  EXPECT_NE(resolver_factory->resolvers.end(),
            resolver_factory->resolvers.find(convert::ToString(test_page_id)));
  ConflictResolverImpl* resolver_impl =
      &(resolver_factory->resolvers.find(convert::ToString(test_page_id))
            ->second);
  ASSERT_EQ(1u, resolver_impl->requests.size());

  // Left change is the most recent, so the one made on |page2|.
  ASSERT_EQ(2u, resolver_impl->requests[0].change_left->changes.size());
  EXPECT_EQ("city",
            convert::ExtendedStringView(
                resolver_impl->requests[0].change_left->changes[0]->key));
  EXPECT_EQ(
      "San Francisco",
      ToString(resolver_impl->requests[0].change_left->changes[0]->value));
  EXPECT_EQ("name",
            convert::ExtendedStringView(
                resolver_impl->requests[0].change_left->changes[1]->key));
  EXPECT_EQ(
      "Alice",
      ToString(resolver_impl->requests[0].change_left->changes[1]->value));

  // Right change comes from |page1|.
  ASSERT_EQ(1u, resolver_impl->requests[0].change_right->changes.size());
  EXPECT_EQ("city",
            convert::ExtendedStringView(
                resolver_impl->requests[0].change_right->changes[0]->key));
  EXPECT_EQ(
      "Paris",
      ToString(resolver_impl->requests[0].change_right->changes[0]->value));
  // Common ancestor is empty.
  PageSnapshotPtr snapshot = PageSnapshotPtr::Create(
      std::move(resolver_impl->requests[0].common_version));
  fidl::Array<EntryPtr> entries =
      SnapshotGetEntries(&snapshot, fidl::Array<uint8_t>());
  EXPECT_EQ(0u, entries.size());

  // Prepare the merged values
  fidl::Array<MergedValuePtr> merged_values =
      fidl::Array<MergedValuePtr>::New(0);
  {
    MergedValuePtr merged_value = MergedValue::New();
    merged_value->key = convert::ToArray("city");
    merged_value->source = ValueSource::RIGHT;
    merged_values.push_back(std::move(merged_value));
  }

  // Watch for the change.
  PageWatcherPtr watcher_ptr;
  Watcher watcher(GetProxy(&watcher_ptr),
                  []() { mtl::MessageLoop::GetCurrent()->PostQuitTask(); });
  PageSnapshotPtr snapshot2;
  page1->GetSnapshot(snapshot2.NewRequest(), std::move(watcher_ptr),
                     [](Status status) { EXPECT_EQ(Status::OK, status); });
  EXPECT_TRUE(page1.WaitForIncomingResponse());

  resolver_impl->requests[0].callback(std::move(merged_values));

  // Wait for the watcher to be called.
  EXPECT_FALSE(RunLoopWithTimeout());

  fidl::Array<EntryPtr> final_entries =
      SnapshotGetEntries(&watcher.last_snapshot_, fidl::Array<uint8_t>());
  ASSERT_EQ(2u, final_entries.size());
  EXPECT_EQ("city", convert::ExtendedStringView(final_entries[0]->key));
  EXPECT_EQ("name", convert::ExtendedStringView(final_entries[1]->key));
}

}  // namespace
}  // namespace integration_tests
}  // namespace ledger
