/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the LICENSE
 * file in the root directory of this source tree.
 */
#include "TestFunctions.h"

namespace facebook {
namespace hermes {
namespace synthtest {

const char *parseGCConfigTrace() {
  return R"###(
{
  "version": 1,
  "globalObjID": 0,
  "gcConfig": {
    "initHeapSize": 100,
    "maxHeapSize": 16777216
  },
  "env": {
    "mathRandomSeed": 0,
    "callsToDateNow": [],
    "callsToNewDate": [],
    "callsToDateAsFunction": []
  },
  "trace": [
    {
      "type": "BeginExecJSRecord",
      "time": 0
    },
    {
      "type": "EndExecJSRecord",
      "time": 0
    }
  ]
}
)###";
}

const char *parseGCConfigSource() {
  // JS doesn't need to run, only need to parse and initialize the GC.
  return "// doesn't matter";
}

} // namespace synthtest
} // namespace hermes
} // namespace facebook