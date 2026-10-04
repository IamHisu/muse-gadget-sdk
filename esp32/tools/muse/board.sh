#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Zhengchen-only build and flash helper.
#   tools/muse/board.sh build|flash zhengchen [serial|port]
set -uo pipefail
cmd=${1:?build|flash}; board=${2:?zhengchen}
root=$(cd "$(dirname "$0")/../.." && pwd)
[ "$board" = zhengchen ] || { echo "unknown board $board"; exit 2; }
target=esp32s3
baud=460800
if [ -n "${IDF_EXPORT:-}" ]; then
    . "$IDF_EXPORT" >/dev/null 2>&1
elif ! command -v idf.py >/dev/null 2>&1; then
    for d in "$HOME/.espressif/esp-idf-v6.0.1" "${IDF_PATH:-}" "$HOME/esp/esp-idf-v6.0.1" "$HOME/esp/esp-idf-v6" "$HOME/esp/esp-idf"; do
        [ -n "$d" ] && [ -f "$d/export.sh" ] && { . "$d/export.sh" >/dev/null 2>&1; break; }
    done
fi
command -v idf.py >/dev/null 2>&1 || { echo "idf.py not found; activate ESP-IDF v6.0.3 first" >&2; exit 1; }
cd "$root"
if [ "$cmd" = build ]; then
    exec idf.py build
fi
port=$(python "$root/tools/muse/ports.py" $board ${3:-}) || exit 1
cd build && python -m esptool --chip $target -p $port -b $baud --before default-reset --after hard-reset write-flash "@flash_args" 2>&1 | tail -3
