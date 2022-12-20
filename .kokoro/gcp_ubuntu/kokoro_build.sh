#!/bin/bash

# Copyright 2022 Centipede Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

# This script is primarily intended to be run by go/Kokoro to build and test
# the OSS variant of Centipede checked out from GitHub. But it can also be run
# manually to build and test the OSS variant locally exported via
# ../../copybara/local_copy.sh. This makes it easy to locally reproduce & debug
# any problems reported by Kokoro.

set -eu

if [[ "${PWD}" =~ google3 ]]; then
  echo "First export OSS variant by running ../../copybara/local_copy.sh, "\
    "then run this script in created ~/centipede_oss"
  kill -9 $$
  exit 1
fi

if [[ -n "${KOKORO_ARTIFACTS_DIR+x}" ]]; then
  # When Kokoro runs the script, it predefines $KOKORO_ARTIFACTS_DIR to point at
  # the checked out GitHub repo, so we just define $KOKORO_CENTIPEDE_DIR.
  declare -r KOKORO_CENTIPEDE_DIR="${KOKORO_ARTIFACTS_DIR}/github/centipede"
  # No need for sudo when run by Kokoro.
  declare -r MAYBE_SUDO=""
else
  # If KOKORO_ARTIFACTS_DIR is undefined, we assume the script is being run
  # manually, and define both $KOKORO_CENTIPEDE_DIR and $KOKORO_ARTIFACTS_DIR.
  KOKORO_ARTIFACTS_DIR="$(cd -L "$(dirname "$0")/../../" && echo "${PWD}")"
  declare -r KOKORO_ARTIFACTS_DIR
  declare -r KOKORO_CENTIPEDE_DIR="${KOKORO_ARTIFACTS_DIR}"
  # Must run under sudo, or else docker trips over insufficient permissions.
  declare -r MAYBE_SUDO="sudo"
fi

# Code under repo is checked out to ${KOKORO_ARTIFACTS_DIR}/github.
# The final directory name in this path is determined by the scm name specified
# in the job configuration.
cd "${KOKORO_CENTIPEDE_DIR}/.kokoro/gcp_ubuntu"

declare -r DOCKER_IMAGE=debian

"${MAYBE_SUDO}" docker run \
  -v "${KOKORO_CENTIPEDE_DIR}:/app" \
  -v "${KOKORO_ARTIFACTS_DIR}:/kokoro" \
  --env KOKORO_ARTIFACTS_DIR=/kokoro \
  -w /app \
  "${DOCKER_IMAGE}" \
  /app/.kokoro/gcp_ubuntu/run_tests_inside_docker.sh
