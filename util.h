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

#ifndef THIRD_PARTY_CENTIPEDE_UTIL_H_
#define THIRD_PARTY_CENTIPEDE_UTIL_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "absl/types/span.h"
#include "./defs.h"
#include "./feature.h"

namespace centipede {

// Resolves an executable path using `which`. Returns the resolved path if
// `path` is resolved (i.e. it is either a valid absolute or relative path or
// can be found via PATH) and is executable by the current user. If the input
// `path` is empty or "/dev/null": `allow_empty`==true returns "";
// `allow_empty`==false CHECKs. If `path` is unresolved:
// `allow_unresolved`==true returns ""; `allow_unresolved`==false CHECKs.
// `description` should say what the path is: it is used in error messages.
// TODO(ussuri): Return StatusOr while doing a global switch to absl::Status.
std::string ResolveExecutablePath(std::string_view path,
                                  std::string_view description,
                                  bool allow_empty, bool allow_unresolved);
// Same as `ResolveExecutablePath`, but for a list of paths.
// TODO(ussuri): Return StatusOr while doing a global switch to absl::Status.
std::vector<std::string> ResolveExecutablePaths(
    const std::vector<std::string_view> &paths, std::string_view description,
    bool allow_empty, bool allow_unresolved);
// CHECKs with a diagnostic if `path` doesn't exist or is not executable.
// TODO(ussuri): Return StatusOr while doing a global switch to absl::Status.
void AssertExecutablePath(std::string_view path);

// Returns a printable hash of a byte array. Currently sha1 is used.
std::string Hash(absl::Span<const uint8_t> span);
// Same as above, but for std::string_view.
std::string Hash(std::string_view str);
// Hashes are always this many bytes.
inline constexpr size_t kHashLen = 40;
// Returns the hash of the contents of the file `file_path`.
std::string HashOfFileContents(std::string_view file_path);
// Returns a printable string representing at most `max_len` bytes of `data`.
std::string AsString(const ByteArray &data, size_t max_len = 16);
// Reads from a local file `file_path` into `data`.
// Crashes on any error.
void ReadFromLocalFile(std::string_view file_path, ByteArray &data);
// Same as above.
void ReadFromLocalFile(std::string_view file_path, std::string &data);
// Same as above but for FeatureVec.
// Crashes if the number of read bytes is not 0 mod sizeof(feature_t).
void ReadFromLocalFile(std::string_view file_path, FeatureVec &data);
// Same as above but for vector<uint32_t>.
void ReadFromLocalFile(std::string_view file_path, std::vector<uint32_t> &data);
// Writes the contents of `data` to a local file `file_path`.
// Crashes on any error.
void WriteToLocalFile(std::string_view file_path,
                      absl::Span<const uint8_t> data);
// Same as above.
void WriteToLocalFile(std::string_view file_path, std::string_view data);
// Same as above but for FeatureVec.
void WriteToLocalFile(std::string_view file_path, const FeatureVec &data);
// Writes `data` to `dir_path`/Hash(`data`). Does nothing if `dir_path.empty()`.
void WriteToLocalHashedFileInDir(std::string_view dir_path,
                                 absl::Span<const uint8_t> data);
// Returns a path string suitable to create a temporary local directory.
// Will return the same value every time it is called within one thread,
// but different values for different threads and difference processes.
std::string TemporaryLocalDirPath();

// Creates an empty dir `path` and schedules it for deletion at exit.
// Uses atexit(), and so the removal will not happen if exit() is bypassed.
// This function is supposed to be called only on paths created by
// TemporaryLocalDirPath().
void CreateLocalDirRemovedAtExit(std::string_view path);

// Requests that the process exits soon, with `exit_code`.
// `exit_code` must be non-zero (!= EXIT_SUCCESS).
// Async-signal-safe.
void RequestEarlyExit(int exit_code);
// Returns true iff RequestEarlyExit() was called.
bool EarlyExitRequested();
// Returns the value most recently passed to RequestEarlyExit()
// or 0 if RequestEarlyExit() was not called.
int ExitCode();

// If `seed` != 0, returns `seed`, otherwise returns a random number
// based on time, pid, tid, etc.
size_t GetRandomSeed(size_t seed);

// Returns a string that starts with `prefix` and that uniquely identifies
// the caller's process and thread.
std::string ProcessAndThreadUniqueID(std::string_view prefix);

// Computes a random subset of `set` that needs to be
// removed to reach `target_size` non-zero weights in the set.
// `set` is an array of weights, some of which could be zero.
//
// Subsets with smaller combined weight are more likely to be returned.
// The return value is a sorted vector of indices of elements of `set`
// that need to be removed, including those with zero weights.
// Randomness is retrieved from `rng`.
//
// Example:  set = {20, 10, 0, 40, 50}
//   For target_size >= 4, the return value will be {2}, i.e. indices of all 0s.
//   For target_size == 3, the return value will be one of
//   {1, 2}, {0, 2}, {2, 3}, {2, 4}, i.e. the indices of 0s and one more index.
//   {1, 2} is more likely than {2, 4} because set[1] < set[4].
//
// This is a flavour of https://en.wikipedia.org/wiki/Reservoir_sampling
// with two differences:
//   * Elements with weight 0 are unconditionally included.
//   * We choose which elements to remove instead of which elements to pick.
//     We use this inverted algorithm because in a typical use case
//     `target_size` is just slightly smaller than set.size().
std::vector<size_t> RandomWeightedSubset(absl::Span<const uint32_t> set,
                                         size_t target_size, Rng &rng);

// Removes all elements from `set` whose indices are found in `subset_indices`.
// `subset_indices` is a sorted vector.
template <typename T>
void RemoveSubset(const std::vector<size_t> &subset_indices,
                  std::vector<T> &set) {
  size_t pos_to_write = 0;
  for (size_t i = 0, n = set.size(); i < n; i++) {
    // If subset_indices.size() is k, this loop's complexity is O(n*log(k)).
    // We can do it in O(n+k) with a bit more code, but this loop is not
    // expected to be hot. Besides, k would typically be small.
    if (!std::binary_search(subset_indices.begin(), subset_indices.end(), i))
      std::swap(set[pos_to_write++], set[i]);
  }
  set.resize(pos_to_write);
}

// Adds a prefix and a postfix to `data` such that the result can be
// appended to another such packed data and then the operation can be reversed.
// The purpose is to allow appending blobs of data to a (possibly remote) file
// such that when reading this file we can separate the blobs.
// TODO(kcc): [impl] is there a lightweight equivalent in the open-source world?
//  tar sounds too heavy.
// TODO(kcc): [impl] investigate https://github.com/google/riegeli.
ByteArray PackBytesForAppendFile(const ByteArray &data);
// Unpacks `packed_data` into `unpacked` and `hashes`.
// `packed_data` is multiple data packed by PackBytesForAppendFile()
// and merged together.
// `unpacked` or `hashes` can be nullptr.
void UnpackBytesFromAppendFile(const ByteArray &packed_data,
                               std::vector<ByteArray> *unpacked,
                               std::vector<std::string> *hashes = nullptr);
// Append the bytes from 'hash' to 'ba'.
void AppendHashToArray(ByteArray &ba, std::string_view hash);
// Reverse to AppendHashToArray.
std::string ExtractHashFromArray(ByteArray &ba);

// Pack {features, Hash(data)} into a byte array.
ByteArray PackFeaturesAndHash(const ByteArray &data,
                              const FeatureVec &features);

// Parses `dictionary_text` representing an AFL/libFuzzer dictionary.
// https://github.com/google/AFL/blob/master/dictionaries/README.dictionaries
// https://llvm.org/docs/LibFuzzer.html#dictionaries
// Fills in `dictionary_entries` with byte sequences from the dictionary.
// Returns true iff parsing completes successfully.
bool ParseAFLDictionary(std::string_view dictionary_text,
                        std::vector<ByteArray> &dictionary_entries);

}  // namespace centipede

#endif  // THIRD_PARTY_CENTIPEDE_UTIL_H_
