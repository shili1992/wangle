/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <folly/futures/Future.h>
#include <wangle/channel/Handler.h>

namespace wangle {

// 能是保证无论从哪一个线程执行write，都可以保证到AsyncSocketHandler时，都处于AsyncSocket所绑定的IO线程
// 一旦使用了EventBaseHandler，那么在EventBaseHandler之后的Handler都可以在任意的线程发起write操作，
// EventBaseHandler会保证最终在IO线程中完成真正的网络IO
class EventBaseHandler : public OutboundBytesToBytesHandler {
 public:
  folly::Future<folly::Unit> write(
      Context* ctx,
      std::unique_ptr<folly::IOBuf> buf) override {
    folly::Future<folly::Unit> retval;
    DCHECK(ctx->getTransport());
    DCHECK(ctx->getTransport()->getEventBase());
    // 先从AsyncSocket中获取它锁绑定的IO线程（EventBase），然后使用
    // runImmediatelyOrRunInEventBaseThreadAndWait直接把事件委派到该线程上执行。
    // 确保无论在哪个Eventbase写，都会重定位到socket所在的eventbase（IO线程）
    // 从其他线程切换到 IO线程进行网络发送
    ctx->getTransport()->getEventBase()->runImmediatelyOrRunInEventBaseThreadAndWait([&](){
        retval = ctx->fireWrite(std::move(buf));
    });
    return retval;
  }

  folly::Future<folly::Unit> close(Context* ctx) override {
    DCHECK(ctx->getTransport());
    DCHECK(ctx->getTransport()->getEventBase());
    folly::Future<folly::Unit> retval;
    ctx->getTransport()->getEventBase()->runImmediatelyOrRunInEventBaseThreadAndWait([&](){
        retval = ctx->fireClose();
    });
    return retval;
  }
};

} // namespace
