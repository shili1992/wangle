/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include <wangle/concurrent/Codel.h>
#include <algorithm>
#include <math.h>

#ifndef NO_LIB_GFLAGS
  #include <gflags/gflags.h>
  DEFINE_int32(codel_interval, 100,
               "Codel default interval time in ms");
  DEFINE_int32(codel_target_delay, 5,
               "Target codel queueing delay in ms");
#endif

namespace folly { namespace wangle {

#ifdef NO_LIB_GFLAGS
  int32_t FLAGS_codel_interval = 100;
  int32_t FLAGS_codel_target_delay = 5;
#endif

Codel::Codel()
    : codelMinDelay_(0),
      codelIntervalTime_(std::chrono::steady_clock::now()),
      codelResetDelay_(true),
      overloaded_(false) {}

bool Codel::overloaded(std::chrono::microseconds delay) {
  bool ret = false;
  auto now = std::chrono::steady_clock::now();

  // Avoid another thread updating the value at the same time we are using it
  // to calculate the overloaded state
  auto minDelay = codelMinDelay_;

  if (now  > codelIntervalTime_ &&
      (!codelResetDelay_.load(std::memory_order_acquire)
       && !codelResetDelay_.exchange(true))) {
    codelIntervalTime_ = now + std::chrono::milliseconds(FLAGS_codel_interval);

    if (minDelay > std::chrono::milliseconds(FLAGS_codel_target_delay)) {
      overloaded_ = true;
    } else {
      overloaded_ = false;
    }
  }
  // Care must be taken that only a single thread resets codelMinDelay_,
  // and that it happens after the interval reset above
  if (codelResetDelay_.load(std::memory_order_acquire) &&
      codelResetDelay_.exchange(false)) {
    codelMinDelay_ = delay;
    // More than one request must come in during an interval before codel
    // starts dropping requests
    return false;
  } else if(delay < codelMinDelay_) {
    codelMinDelay_ = delay;
  }

  if (overloaded_ &&
      delay > std::chrono::milliseconds(FLAGS_codel_target_delay * 2)) {
    ret = true;
  }

  return ret;

}

int Codel::getLoad() {
  return std::min(100, (int)codelMinDelay_.count() /
                  (2 * FLAGS_codel_target_delay));
}

int Codel::getMinDelay() {
  return (int) codelMinDelay_.count();
}

}} //namespace
