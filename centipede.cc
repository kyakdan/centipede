// Copyright 2022 The Centipede Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Centipede: an experimental distributed fuzzing engine.
// Very simple / naive so far.
// Main use case: large out-of-process fuzz targets with relatively slow
// execution (< 100 exec/s).
//
// Basic approach (subject to change):
// * All state is stored in a local or remote directory `workdir`.
// * State consists of a corpus (inputs) and feature sets (see feature_t).
// * Feature sets are associated with a binary, so that two binaries
//   have independent feature sets stored in different subdirs in `workdir`,
//   like binaryA-sha1-of-A and binaryB-sha1-of-B.
//   If the binary is recompiled at different revision or with different
//   compiler options, it is a different binary and feature sets will need to be
//   recomputed for the new binary in its separate dir.
// * The corpus is not tied to the binary. It is stored in `workdir`/.
// * The fuzzer runs in `total_shards` independent processes.
// * Each shard appends data to its own files in `workdir`: corpus and features;
//   no other process writes to those files.
// * Each shard may periodically read some other shard's corpus and features.
//   Since all files are append-only (no renames, no deletions) we may only
//   have partial reads, and the algorithm is expected to tolerate those.
// * Fuzzing can be run locally in multiple processes, with a local `workdir`
//   or on a cluster, which supports `workdir` on a remote file system.
// * The intent is to scale to an arbitrary number of shards,
//   currently tested with total_shards = 10000.
//
//  Differential fuzzing is not yet properly implemented.
//  Currently one can run target A in a given workdir, then target B, and so on,
//  and the corpus will grow over time benefiting from all targets.
#include "./centipede.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <iostream>
#include <memory>
#include <numeric>
#include <string>
#include <string_view>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "absl/synchronization/mutex.h"
#include "absl/types/span.h"
#include "./blob_file.h"
#include "./control_flow.h"
#include "./coverage.h"
#include "./defs.h"
#include "./environment.h"
#include "./execution_result.h"
#include "./feature.h"
#include "./logging.h"
#include "./remote_file.h"
#include "./rusage_profiler.h"
#include "./rusage_stats.h"
#include "./shard_reader.h"
#include "./util.h"

namespace centipede {

using perf::RUsageProfiler;

Centipede::Centipede(const Environment &env, CentipedeCallbacks &user_callbacks,
                     const BinaryInfo &binary_info,
                     CoverageLogger &coverage_logger, Stats &stats)
    : env_(env),
      user_callbacks_(user_callbacks),
      rng_(env_.seed),
      // TODO(kcc): [impl] find a better way to compute frequency_threshold.
      fs_(env_.feature_frequency_threshold),
      coverage_frontier_(binary_info),
      pc_table_(binary_info.pc_table),
      symbols_(binary_info.symbols),
      binary_info_(binary_info),
      function_filter_(env_.function_filter, symbols_),
      coverage_logger_(coverage_logger),
      stats_(stats),
      input_filter_path_(std::filesystem::path(TemporaryLocalDirPath())
                             .append("filter-input")),
      input_filter_cmd_(env_.input_filter, {input_filter_path_}, {/*env*/},
                        "/dev/null", "/dev/null"),
      rusage_profiler_(
          /*scope=*/perf::RUsageScope::ThisProcess(),
          /*metrics=*/env.DumpRUsageTelemetryInThisShard()
              ? RUsageProfiler::kAllMetrics
              : RUsageProfiler::kMetricsOff,
          /*raii_actions=*/RUsageProfiler::kRaiiOff,
          /*location=*/{__FILE__, __LINE__},
          /*description=*/"Engine") {
  CHECK(env_.seed) << "env_.seed must not be zero";
  if (!env_.input_filter.empty() && env_.fork_server)
    input_filter_cmd_.StartForkServer(TemporaryLocalDirPath(), "input_filter");
}

int Centipede::SaveCorpusToLocalDir(const Environment &env,
                                    std::string_view save_corpus_to_local_dir) {
  for (size_t shard = 0; shard < env.total_shards; shard++) {
    auto reader = DefaultBlobFileReaderFactory();
    reader->Open(env.MakeCorpusPath(shard)).IgnoreError();  // may not exist.
    absl::Span<uint8_t> blob;
    size_t num_read = 0;
    while (reader->Read(blob).ok()) {
      ++num_read;
      WriteToLocalHashedFileInDir(save_corpus_to_local_dir, blob);
    }
    LOG(INFO) << "Read " << num_read << " from " << env.MakeCorpusPath(shard);
  }
  return 0;
}

int Centipede::ExportCorpusFromLocalDir(const Environment &env,
                                        std::string_view local_dir) {
  // Shard the file paths in `local_dir` based on hashes of filenames.
  // Such partition is stable: a given file always goes to a specific shard.
  std::vector<std::vector<std::string>> sharded_paths(env.total_shards);
  size_t total_paths = 0;
  for (const auto &entry :
       std::filesystem::recursive_directory_iterator(local_dir)) {
    if (entry.is_regular_file()) {
      size_t filename_hash = std::hash<std::string>{}(entry.path().filename());
      sharded_paths[filename_hash % env.total_shards].push_back(entry.path());
      ++total_paths;
    }
  }
  // Iterate over all shards.
  size_t inputs_added = 0;
  size_t inputs_ignored = 0;
  for (size_t shard = 0; shard < env.total_shards; shard++) {
    size_t num_shard_bytes = 0;
    // Read the shard (if it exists), collect input hashes from it.
    absl::flat_hash_set<std::string> existing_hashes;
    {
      auto reader = DefaultBlobFileReaderFactory();
      // May fail to open if file doesn't exist.
      reader->Open(env.MakeCorpusPath(shard)).IgnoreError();
      absl::Span<uint8_t> blob;
      while (reader->Read(blob).ok()) {
        existing_hashes.insert(Hash(blob));
      }
    }
    // Add inputs to the current shard, if the shard doesn't have them already.
    auto appender = DefaultBlobFileAppenderFactory();
    std::string corpus_path = env.MakeCorpusPath(shard);
    CHECK_OK(appender->Open(corpus_path))
        << "Failed to open corpus file: " << corpus_path;
    ByteArray shard_data;
    for (const auto &path : sharded_paths[shard]) {
      ByteArray input;
      ReadFromLocalFile(path, input);
      if (input.empty() || existing_hashes.contains(Hash(input))) {
        ++inputs_ignored;
        continue;
      }
      CHECK_OK(appender->Append(input));
      ++inputs_added;
    }
    LOG(INFO) << VV(shard) << VV(inputs_added) << VV(inputs_ignored)
              << VV(num_shard_bytes) << VV(shard_data.size());
  }
  CHECK_EQ(total_paths, inputs_added + inputs_ignored);
  return 0;
}

void Centipede::UpdateAndMaybeLogStats(std::string_view log_type,
                                       size_t min_log_level) {
  stats_.corpus_size = corpus_.NumActive();
  stats_.num_covered_pcs = fs_.ToCoveragePCs().size();

  if (env_.log_level < min_log_level) return;

  const double fuzz_time_secs =
      absl::ToDoubleSeconds(absl::Now() - fuzz_start_time_);
  // NOTE: By construction, if `fuzz_time_secs` <= 0, then the actual fuzzing
  // hasn't started yet.
  double execs_per_sec =
      fuzz_time_secs > 0 ? static_cast<double>(num_runs_) / fuzz_time_secs : 0;
  if (execs_per_sec > 1.) execs_per_sec = std::round(execs_per_sec);
  auto [max_corpus_size, avg_corpus_size] = corpus_.MaxAndAvgSize();
  static const auto rusage_scope = perf::RUsageScope::ThisProcess();
  LOG(INFO) << env_.experiment_name << "[" << num_runs_ << "]"
            << " " << log_type << ":"
            << " ft: " << fs_.size() << " cov: " << fs_.ToCoveragePCs().size()
            << " cnt: " << fs_.CountFeatures(feature_domains::k8bitCounters)
            << " df: " << fs_.CountFeatures(feature_domains::kDataFlow)
            << " cmp: " << fs_.CountFeatures(feature_domains::kCMP)
            << " path: " << fs_.CountFeatures(feature_domains::kBoundedPath)
            << " pair: " << fs_.CountFeatures(feature_domains::kPCPair)
            << " corp: " << corpus_.NumActive() << "/" << corpus_.NumTotal()
            << " fr: " << coverage_frontier_.NumFunctionsInFrontier()
            << " max/avg: " << max_corpus_size << "/" << avg_corpus_size << " "
            << corpus_.MemoryUsageString() << " exec/s: " << execs_per_sec
            << " mb: "
            << (perf::RUsageMemory::Snapshot(rusage_scope).mem_rss >> 20);
}

void Centipede::LogFeaturesAsSymbols(const FeatureVec &fv) {
  if (!env_.LogFeaturesInThisShard()) return;
  auto feature_domain = feature_domains::k8bitCounters;
  for (auto feature : fv) {
    if (!feature_domain.Contains(feature)) continue;
    PCIndex pc_index = Convert8bitCounterFeatureToPcIndex(feature);
    auto description = coverage_logger_.ObserveAndDescribeIfNew(pc_index);
    if (description.empty()) continue;
    LOG(INFO) << description;
  }
}

bool Centipede::InputPassesFilter(const ByteArray &input) {
  if (env_.input_filter.empty()) return true;
  WriteToLocalFile(input_filter_path_, input);
  bool result = input_filter_cmd_.Execute() == EXIT_SUCCESS;
  std::filesystem::remove(input_filter_path_);
  return result;
}

bool Centipede::ExecuteAndReportCrash(std::string_view binary,
                                      const std::vector<ByteArray> &input_vec,
                                      BatchResult &batch_result) {
  bool success = user_callbacks_.Execute(binary, input_vec, batch_result);
  if (!success) ReportCrash(binary, input_vec, batch_result);
  return success;
}

// *** Highly experimental and risky. May not scale well for large targets. ***
//
// The idea: an unordered pair of two features {a, b} is by itself a feature.
// In the worst case, the number of such synthetic features is a square of
// the number of regular features, which may not scale.
// For now, we only treat pairs of PCs as features, which is still quadratic
// by the number of PCs. But in moderate-sized programs this may be tolerable.
//
// Rationale: if two different parts of the target are exercised simultaneously,
// this may create interesting behaviour that is hard to capture with regular
// control flow (or other) features.
size_t Centipede::AddPcPairFeatures(FeatureVec &fv) {
  // Using a scratch vector to avoid allocations.
  auto &pcs = add_pc_pair_scratch_;
  pcs.clear();

  size_t num_pcs = pc_table_.size();
  size_t num_added_pairs = 0;

  // Collect PCs from fv.
  for (auto feature : fv) {
    if (feature_domains::k8bitCounters.Contains(feature))
      pcs.push_back(Convert8bitCounterFeatureToPcIndex(feature));
  }

  // The quadratic loop: iterate all PC pairs (!!).
  for (size_t i = 0, n = pcs.size(); i < n; ++i) {
    size_t pc1 = pcs[i];
    for (size_t j = i + 1; j < n; ++j) {
      size_t pc2 = pcs[j];
      feature_t f = feature_domains::kPCPair.ConvertToMe(
          ConvertPcPairToNumber(pc1, pc2, num_pcs));
      // If we have seen this pair at least once, ignore it.
      if (fs_.Frequency(f)) continue;
      fv.push_back(f);
      ++num_added_pairs;
    }
  }
  return num_added_pairs;
}

bool Centipede::RunBatch(const std::vector<ByteArray> &input_vec,
                         BlobFileAppender *corpus_file,
                         BlobFileAppender *features_file,
                         BlobFileAppender *unconditional_features_file) {
  BatchResult batch_result;
  bool success = ExecuteAndReportCrash(env_.binary, input_vec, batch_result);
  CHECK_EQ(input_vec.size(), batch_result.results().size());

  for (const auto &extra_binary : env_.extra_binaries) {
    BatchResult extra_batch_result;
    success =
        ExecuteAndReportCrash(extra_binary, input_vec, extra_batch_result) &&
        success;
  }
  if (!success && env_.exit_on_crash) {
    LOG(INFO) << "--exit_on_crash is enabled; exiting soon";
    RequestEarlyExit(1);
    return false;
  }
  CHECK_EQ(batch_result.results().size(), input_vec.size());
  num_runs_ += input_vec.size();
  bool batch_gained_new_coverage = false;
  for (size_t i = 0; i < input_vec.size(); i++) {
    if (EarlyExitRequested()) break;
    FeatureVec &fv = batch_result.results()[i].mutable_features();
    bool function_filter_passed = function_filter_.filter(fv);
    bool input_gained_new_coverage =
        fs_.CountUnseenAndPruneFrequentFeatures(fv);
    if (env_.use_pcpair_features && AddPcPairFeatures(fv))
      input_gained_new_coverage = true;
    if (unconditional_features_file) {
      CHECK_OK(unconditional_features_file->Append(
          PackFeaturesAndHash(input_vec[i], fv)));
    }
    if (input_gained_new_coverage) {
      // TODO(kcc): [impl] add stats for filtered-out inputs.
      if (!InputPassesFilter(input_vec[i])) continue;
      fs_.IncrementFrequencies(fv);
      LogFeaturesAsSymbols(fv);
      batch_gained_new_coverage = true;
      CHECK_GT(fv.size(), 0UL);
      if (function_filter_passed) {
        const auto &cmp_args = batch_result.results()[i].cmp_args();
        corpus_.Add(input_vec[i], fv, cmp_args, fs_, coverage_frontier_);
      }
      if (corpus_file) {
        CHECK_OK(corpus_file->Append(input_vec[i]));
      }
      if (!env_.corpus_dir.empty()) {
        WriteToLocalHashedFileInDir(env_.corpus_dir[0], input_vec[i]);
      }
      if (features_file) {
        CHECK_OK(features_file->Append(PackFeaturesAndHash(input_vec[i], fv)));
      }
    }
  }
  return batch_gained_new_coverage;
}

// TODO(kcc): [impl] don't reread the same corpus twice.
void Centipede::LoadShard(const Environment &load_env, size_t shard_index,
                          bool rerun) {
  size_t added_to_corpus = 0;
  std::vector<ByteArray> to_rerun;
  auto input_features_callback = [&](const ByteArray &input,
                                     FeatureVec &features) {
    if (EarlyExitRequested()) return;
    if (features.empty()) {
      if (rerun) {
        to_rerun.push_back(input);
      }
    } else {
      LogFeaturesAsSymbols(features);
      if (fs_.CountUnseenAndPruneFrequentFeatures(features)) {
        fs_.IncrementFrequencies(features);
        // TODO(kcc): cmp_args are currently not saved to disk and not reloaded.
        corpus_.Add(input, features, {}, fs_, coverage_frontier_);
        added_to_corpus++;
      }
    }
  };

  // See serialize_shard_loads on why we may want to serialize shard loads.
  // TODO(kcc): remove serialize_shard_loads when LoadShards() uses less RAM.
  if (env_.serialize_shard_loads) {
    ABSL_CONST_INIT static absl::Mutex load_shard_mu{absl::kConstInit};
    absl::MutexLock lock(&load_shard_mu);
    ReadShard(load_env.MakeCorpusPath(shard_index),
              load_env.MakeFeaturesPath(shard_index), input_features_callback);
  } else {
    ReadShard(load_env.MakeCorpusPath(shard_index),
              load_env.MakeFeaturesPath(shard_index), input_features_callback);
  }
  if (added_to_corpus) UpdateAndMaybeLogStats("load-shard", 1);
  Rerun(to_rerun);
}

void Centipede::Rerun(std::vector<ByteArray> &to_rerun) {
  if (to_rerun.empty()) return;
  auto features_file = DefaultBlobFileAppenderFactory();
  CHECK_OK(features_file->Open(env_.MakeFeaturesPath(env_.my_shard_index)));

  LOG(INFO) << to_rerun.size() << " inputs to rerun";
  // Re-run all inputs for which we don't know their features.
  // Run in batches of at most env_.batch_size inputs each.
  while (!to_rerun.empty()) {
    if (EarlyExitRequested()) break;
    size_t batch_size = std::min(to_rerun.size(), env_.batch_size);
    std::vector<ByteArray> batch(to_rerun.end() - batch_size, to_rerun.end());
    to_rerun.resize(to_rerun.size() - batch_size);
    if (RunBatch(batch, nullptr, nullptr, features_file.get())) {
      UpdateAndMaybeLogStats("rerun-old", 1);
    }
  }
}

void Centipede::GenerateCoverageReport(std::string_view annotation,
                                       size_t batch_index) {
  if (pc_table_.empty()) return;

  auto pci_vec = fs_.ToCoveragePCs();
  Coverage coverage(pc_table_, pci_vec);
  std::stringstream out;
  out << "# Last batch: " << batch_index << "\n\n";
  coverage.Print(symbols_, out);
  // Repackage the output as ByteArray for RemoteFileAppend's consumption.
  // TODO(kcc): [impl] may want to introduce RemoteFileAppend(f, std::string).
  std::string str = out.str();
  ByteArray bytes(str.begin(), str.end());
  auto coverage_path = env_.MakeCoverageReportPath(annotation);
  LOG(INFO) << "Generate coverage report: " << VV(batch_index)
            << VV(coverage_path);
  auto f = RemoteFileOpen(coverage_path, "w");
  CHECK(f);
  RemoteFileAppend(f, bytes);
  RemoteFileClose(f);
}

void Centipede::GenerateCorpusStats(std::string_view annotation,
                                    size_t batch_index) {
  std::ostringstream os;
  os << "# Last batch: " << batch_index << "\n\n";
  corpus_.PrintStats(os, fs_);
  std::string str = os.str();
  ByteArray bytes(str.begin(), str.end());
  auto stats_path = env_.MakeCorpusStatsPath(annotation);
  LOG(INFO) << "Generate corpus stats: " << VV(batch_index) << VV(stats_path);
  auto *f = RemoteFileOpen(stats_path, "w");
  CHECK(f);
  RemoteFileAppend(f, bytes);
  RemoteFileClose(f);
}

// TODO(nedwill): add integration test once tests are refactored per b/255660879
void Centipede::GenerateSourceBasedCoverageReport(std::string_view annotation,
                                                  size_t batch_index) {
  if (env_.clang_coverage_binary.empty()) return;

  auto report_path = env_.MakeSourceBasedCoverageReportPath(annotation);
  RemoteMkdir(report_path);

  std::vector<std::string> raw_profiles = env_.EnumerateRawCoverageProfiles();

  if (raw_profiles.empty()) {
    LOG(ERROR) << "No raw profiles found for coverage report";
    return;
  }

  std::string indexed_profile_path =
      env_.MakeSourceBasedCoverageIndexedProfilePath();

  std::vector<std::string> merge_arguments = {"merge", "-o",
                                              indexed_profile_path, "-sparse"};
  for (const std::string &raw_profile : raw_profiles) {
    merge_arguments.push_back(raw_profile);
  }

  Command merge_command("llvm-profdata", merge_arguments);
  if (merge_command.Execute()) {
    LOG(ERROR) << "Failed to run command " << merge_command.ToString();
    return;
  }

  Command generate_report_command(
      "llvm-cov",
      {"show", "-format=html", absl::StrCat("-output-dir=", report_path),
       absl::StrCat("-instr-profile=", indexed_profile_path),
       env_.clang_coverage_binary});
  if (generate_report_command.Execute()) {
    LOG(ERROR) << "Failed to run command "
               << generate_report_command.ToString();
    return;
  }
}

void Centipede::GenerateRUsageReport(std::string_view annotation,
                                     size_t batch_index) {
  class ReportDumper : public RUsageProfiler::ReportSink {
   public:
    explicit ReportDumper(std::string_view path)
        : file_{RemoteFileOpen(path, "w")} {}

    ~ReportDumper() override { RemoteFileClose(file_); }

    ReportDumper &operator<<(const std::string &fragment) override {
      RemoteFileAppend(file_, ByteArray{fragment.cbegin(), fragment.cend()});
      return *this;
    }

   private:
    RemoteFile *file_;
  };

  const auto description = absl::StrCat("Batch ", batch_index);
  const auto &snapshot =
      rusage_profiler_.TakeSnapshot({__FILE__, __LINE__}, description);
  VLOG(1) << "Rusage @ " << description << ": " << snapshot.ShortMetricsStr();
  // The very first call with `batch_index`==0 is for the initial state: just
  // take a baseline snapshot, but skip the report.
  if (batch_index > 0) {
    auto path = env_.MakeRUsageReportPath(annotation);
    LOG(INFO) << "Generate rusage report: " << VV(env_.my_shard_index)
              << VV(batch_index) << VV(path);
    ReportDumper dumper{path};
    rusage_profiler_.GenerateReport(&dumper);
  }
}

void Centipede::MaybeGenerateTelemetry(std::string_view annotation,
                                       size_t batch_index) {
  if (env_.DumpTelemetryForThisBatch(batch_index)) {
    if (env_.DumpCorpusTelemetryInThisShard()) {
      GenerateCoverageReport(annotation, batch_index);
      GenerateCorpusStats(annotation, batch_index);
      GenerateSourceBasedCoverageReport(annotation, batch_index);
    }
    if (env_.DumpRUsageTelemetryInThisShard()) {
      GenerateRUsageReport(annotation, batch_index);
    }
  }
}

void Centipede::MergeFromOtherCorpus(std::string_view merge_from_dir,
                                     size_t shard_index_to_merge) {
  LOG(INFO) << __func__ << ": " << merge_from_dir;
  Environment merge_from_env = env_;
  merge_from_env.workdir = merge_from_dir;
  size_t initial_corpus_size = corpus_.NumActive();
  LoadShard(merge_from_env, shard_index_to_merge, /*rerun=*/true);
  size_t new_corpus_size = corpus_.NumActive();
  CHECK_GE(new_corpus_size, initial_corpus_size);  // Corpus can't shrink here.
  if (new_corpus_size > initial_corpus_size) {
    auto appender = DefaultBlobFileAppenderFactory();
    CHECK_OK(appender->Open(env_.MakeCorpusPath(env_.my_shard_index)));
    for (size_t idx = initial_corpus_size; idx < new_corpus_size; ++idx) {
      CHECK_OK(appender->Append(corpus_.Get(idx)));
    }
    LOG(INFO) << "Merge: " << (new_corpus_size - initial_corpus_size)
              << " new inputs added";
  }
}

void Centipede::FuzzingLoop() {
  LOG(INFO) << "Shard: " << env_.my_shard_index << "/" << env_.total_shards
            << " " << TemporaryLocalDirPath() << " "
            << "seed: " << env_.seed << "\n\n\n";

  {
    // Execute a dummy input.
    BatchResult batch_result;
    user_callbacks_.Execute(env_.binary, {user_callbacks_.DummyValidInput()},
                            batch_result);
  }

  UpdateAndMaybeLogStats("begin-fuzz", 0);

  if (env_.full_sync || env_.DistillingInThisShard()) {
    // Load all shards in random order.
    std::vector<size_t> shards(env_.total_shards);
    std::iota(shards.begin(), shards.end(), 0);
    std::shuffle(shards.begin(), shards.end(), rng_);
    size_t num_shards_loaded = 0;
    for (auto shard : shards) {
      LoadShard(env_, shard, /*rerun=*/shard == env_.my_shard_index);
      // Log every 100 shards.
      LOG_IF(INFO, (++num_shards_loaded % 100) == 0) << VV(num_shards_loaded);
    }
  } else {
    // Only load my shard.
    LoadShard(env_, env_.my_shard_index, /*rerun=*/true);
  }

  if (!env_.merge_from.empty()) {
    // Merge a shard with the same index from another corpus.
    MergeFromOtherCorpus(env_.merge_from, env_.my_shard_index);
  }

  auto corpus_file = DefaultBlobFileAppenderFactory();
  auto features_file = DefaultBlobFileAppenderFactory();
  CHECK_OK(corpus_file->Open(env_.MakeCorpusPath(env_.my_shard_index)));
  CHECK_OK(features_file->Open(env_.MakeFeaturesPath(env_.my_shard_index)));

  if (corpus_.NumTotal() == 0)
    corpus_.Add(user_callbacks_.DummyValidInput(), {}, {}, fs_,
                coverage_frontier_);

  UpdateAndMaybeLogStats("init-done", 0);

  // Clear fuzz_start_time_ and num_runs_, so that the pre-init work doesn't
  // affect them.
  fuzz_start_time_ = absl::Now();
  num_runs_ = 0;

  if (env_.DistillingInThisShard()) {
    auto distill_to_path = env_.MakeDistilledPath();
    auto appender = DefaultBlobFileAppenderFactory();
    CHECK_OK(appender->Open(distill_to_path));
    for (size_t i = 0; i < corpus_.NumActive(); i++) {
      CHECK_OK(appender->Append(corpus_.Get(i)));
      if (!env_.corpus_dir.empty()) {
        WriteToLocalHashedFileInDir(env_.corpus_dir[0], corpus_.Get(i));
      }
    }
    LOG(INFO) << "distill_to_path: " << distill_to_path
              << " distilled_size: " << corpus_.NumActive();
  }

  // Dump the initial telemetry files. For a brand-new run, these will be
  // functionally empty, e.g. the coverage report will list all target functions
  // as not covered (NONE). For a bootstrapped run (the workdir already has
  // data), these may or may not coincide with the final "latest" report of the
  // previous run, depending on how the runs are configured (the same number of
  // shards, for example).
  MaybeGenerateTelemetry("initial", /*batch_index=*/0);

  // num_runs / batch_size, rounded up.
  size_t number_of_batches = env_.num_runs / env_.batch_size;
  if (env_.num_runs % env_.batch_size != 0) ++number_of_batches;
  size_t new_runs = 0;
  size_t corpus_size_at_last_prune = corpus_.NumActive();
  for (size_t batch_index = 0; batch_index < number_of_batches; batch_index++) {
    if (EarlyExitRequested()) break;
    CHECK_LT(new_runs, env_.num_runs);
    auto remaining_runs = env_.num_runs - new_runs;
    auto batch_size = std::min(env_.batch_size, remaining_runs);
    std::vector<ByteArray> inputs, mutants;
    inputs.resize(env_.mutate_batch_size);
    for (size_t i = 0; i < env_.mutate_batch_size; i++) {
      const auto &corpus_record = env_.use_corpus_weights
                                      ? corpus_.WeightedRandom(rng_())
                                      : corpus_.UniformRandom(rng_());
      inputs[i] = corpus_record.data;
      // Use the cmp_args of the first input.
      // See the related TODO around SetCmpDictionary.
      if (i == 0) user_callbacks_.SetCmpDictionary(corpus_record.cmp_args);
    }

    user_callbacks_.Mutate(inputs, batch_size, mutants);
    bool gained_new_coverage =
        RunBatch(mutants, corpus_file.get(), features_file.get(), nullptr);
    new_runs += mutants.size();

    if (gained_new_coverage) {
      UpdateAndMaybeLogStats("new-feature", 1);
    } else if (((batch_index - 1) & batch_index) == 0) {
      // Log if batch_index is a power of two.
      UpdateAndMaybeLogStats("pulse", 1);
    }

    // Dump the intermediate telemetry files.
    MaybeGenerateTelemetry("latest", batch_index);

    if (env_.load_other_shard_frequency != 0 && batch_index != 0 &&
        (batch_index % env_.load_other_shard_frequency) == 0 &&
        env_.total_shards > 1) {
      size_t rand = rng_() % (env_.total_shards - 1);
      size_t other_shard_index =
          (env_.my_shard_index + 1 + rand) % env_.total_shards;
      CHECK_NE(other_shard_index, env_.my_shard_index);
      LoadShard(env_, other_shard_index, /*rerun=*/false);
    }

    // Prune if we added enough new elements since last prune.
    if (env_.prune_frequency &&
        corpus_.NumActive() >
            corpus_size_at_last_prune + env_.prune_frequency) {
      if (env_.use_coverage_frontier) coverage_frontier_.Compute(corpus_);
      corpus_.Prune(fs_, coverage_frontier_, env_.max_corpus_size, rng_);
      corpus_size_at_last_prune = corpus_.NumActive();
    }
  }

  // Dump the final telemetry files, possibly overwriting the last intermediate
  // version dumped inside the loop.
  MaybeGenerateTelemetry("latest", number_of_batches);

  UpdateAndMaybeLogStats(
      "end-fuzz", 0);  // Tests rely on this line being present at the end.
}

void Centipede::ReportCrash(std::string_view binary,
                            const std::vector<ByteArray> &input_vec,
                            const BatchResult &batch_result) {
  if (num_crash_reports_ >= env_.max_num_crash_reports) return;

  std::string log_prefix =
      absl::StrCat("ReportCrash[", num_crash_reports_, "]: ");

  LOG(INFO) << log_prefix << "Batch execution failed:"
            << "\nBinary          : " << binary
            << "\nExit code       : " << batch_result.exit_code()
            << "\nFailure         : " << batch_result.failure_description()
            << "\nNumber of inputs: " << input_vec.size()
            << "\nCrash log       :\n\n";
  for (const auto &log_line :
       absl::StrSplit(absl::StripAsciiWhitespace(batch_result.log()), '\n')) {
    LOG(INFO).NoPrefix() << "CRASH LOG: " << log_line;
  }
  LOG(INFO).NoPrefix() << "\n";

  LOG_IF(INFO, ++num_crash_reports_ == env_.max_num_crash_reports)
      << log_prefix
      << "Reached --max_num_crash_reports: further reports will be suppressed";

  // Determine the optimal order of the inputs to try to maximize the chances of
  // finding the reproducer fast.
  CHECK_EQ(input_vec.size(), batch_result.results().size());
  std::deque<size_t> input_idxs_to_try(input_vec.size());
  std::iota(input_idxs_to_try.begin(), input_idxs_to_try.end(), 0);
  // Prioritize the presumed crasher by inserting it in front of everything
  // else. However, do keep it at the old location, too, in case the target was
  // primed for a crash by the sequence of inputs that preceded the crasher.
  const size_t suspect_input_idx = batch_result.num_outputs_read();
  if (suspect_input_idx < input_vec.size()) {
    input_idxs_to_try.push_front(suspect_input_idx);
  }

  // Try inputs one-by-one in the determined order.
  LOG(INFO) << log_prefix
            << "Executing inputs one-by-one, trying to find the reproducer";
  for (auto input_idx : input_idxs_to_try) {
    const auto &one_input = input_vec[input_idx];
    BatchResult one_input_batch_result;
    if (!user_callbacks_.Execute(binary, {one_input}, one_input_batch_result)) {
      auto hash = Hash(one_input);
      auto crash_dir = env_.MakeCrashReproducerDirPath();
      RemoteMkdir(crash_dir);
      std::string file_path = std::filesystem::path(crash_dir).append(hash);
      LOG(INFO) << log_prefix << "Detected crash-reproducing input:"
                << "\nInput bytes    : " << AsString(one_input, /*max_len=*/32)
                << "\nExit code      : " << one_input_batch_result.exit_code()
                << "\nFailure        : "
                << one_input_batch_result.failure_description()
                << "\nSaving input to: " << file_path;
      auto *file = RemoteFileOpen(file_path, "w");  // overwrites existing file.
      CHECK(file != nullptr) << log_prefix << "Failed to open " << file_path;
      RemoteFileAppend(file, one_input);
      RemoteFileClose(file);
      return;
    }
  }

  LOG(INFO) << log_prefix
            << "Crash was not observed when running inputs one-by-one";
  // TODO(kcc): [as-needed] there will be cases when several inputs cause a
  // crash, but no single input does. Handle this case.
}

}  // namespace centipede
