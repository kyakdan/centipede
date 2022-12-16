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

#include "./runner_utils.h"

#include <cstdio>
#include <cstdlib>

namespace centipede {

void PrintErrorAndExitIf(bool condition, const char *error) {
  if (!condition) return;
  fprintf(stderr, "error: %s\n", error);
  exit(1);
}

}  // namespace centipede
