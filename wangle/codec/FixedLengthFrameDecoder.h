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

#include <wangle/codec/ByteToMessageCodec.h>

namespace folly {namespace wangle {

/**
 * A decoder that splits the received IOBufs by the fixed number
 * of bytes. For example, if you received the following four
 * fragmented packets:
 *
 * +---+----+------+----+
 * | A | BC | DEFG | HI |
 * +---+----+------+----+
 *
 * A FixedLengthFrameDecoder will decode them into the following three
 * packets with the fixed length:
 *
 * +-----+-----+-----+
 * | ABC | DEF | GHI |
 * +-----+-----+-----+
 *
 */
class FixedLengthFrameDecoder
  : public ByteToMessageCodec {
 public:

  FixedLengthFrameDecoder(size_t length)
    : length_(length) {}

  std::unique_ptr<IOBuf> decode(Context* ctx, IOBufQueue& q, size_t& needed) {
    if (q.chainLength() < length_) {
      needed = length_ - q.chainLength();
      return nullptr;
    }

    return q.split(length_);
  }

 private:
  size_t length_;
};

}} // Namespace
