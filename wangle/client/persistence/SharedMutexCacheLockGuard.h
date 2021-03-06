/*
 *  Copyright (c) 2016, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#pragma once

#include <folly/SharedMutex.h>
#include <wangle/client/persistence/FilePersistentCache.h>

namespace wangle {

// A CacheLockGuard specialization for a folly::SharedMutex
template<>
struct CacheLockGuard<folly::SharedMutex> {
  using Read = folly::SharedMutex::ReadHolder;
  using Write = folly::SharedMutex::WriteHolder;
};

}
