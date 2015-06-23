/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#pragma once
#include <folly/Executor.h>

#include <thread>

namespace folly { namespace wangle {

class ThreadFactory {
 public:
  virtual ~ThreadFactory() {}
  virtual std::thread newThread(Func&& func) = 0;
};

}} // folly::wangle
