// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef APPS_LEDGER_BENCHMARK_SYNC_SYNC_H_
#define APPS_LEDGER_BENCHMARK_SYNC_SYNC_H_

#include <memory>

#include "application/lib/app/application_context.h"
#include "apps/ledger/services/public/ledger.fidl.h"
#include "lib/ftl/files/scoped_temp_dir.h"

namespace benchmark {

// Benchmark that measures sync latency between two Ledger instances syncing
// through the cloud. This emulates syncing between devices, as the Ledger
// instances have separate disk storage.
//
// Cloud sync needs to be configured on the device in order for the benchmark to
// run.
//
// Parameters:
//   --entry-count=<int> the number of entries to be put
//   --value-size=<int> the size of a single value in bytes
class SyncBenchmark : public ledger::PageWatcher {
 public:
  SyncBenchmark(int entry_count, int value_size);

  void Run();

  // ledger::PageWatcher:
  void OnChange(ledger::PageChangePtr page_change,
                ledger::ResultState result_state,
                const OnChangeCallback& callback) override;

 private:
  void RunSingle(int i);

  void ShutDown();

  std::unique_ptr<app::ApplicationContext> application_context_;
  int entry_count_;
  fidl::Array<uint8_t> value_;
  fidl::Binding<ledger::PageWatcher> page_watcher_binding_;
  files::ScopedTempDir alpha_tmp_dir_;
  files::ScopedTempDir beta_tmp_dir_;
  app::ApplicationControllerPtr alpha_controller_;
  app::ApplicationControllerPtr beta_controller_;
  bool alpha_ready_ = false;
  bool beta_ready_ = false;
  ledger::PagePtr alpha_page_;
  ledger::PagePtr beta_page_;

  FTL_DISALLOW_COPY_AND_ASSIGN(SyncBenchmark);
};

}  // namespace benchmark

#endif  // APPS_LEDGER_BENCHMARK_SYNC_SYNC_H_
