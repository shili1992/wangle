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

#include <wangle/channel/Handler.h>
#include <folly/io/async/AsyncSocket.h>
#include <folly/io/async/EventBase.h>
#include <folly/io/async/EventBaseManager.h>
#include <folly/io/IOBuf.h>
#include <folly/io/IOBufQueue.h>

namespace wangle {

// This handler may only be used in a single Pipeline
//    上层业务调用了pipeline->wirte方法之后，便会在Pipeline中引发一个Outbound类型的事件传播，
//    这个事件会从back_链表表头开始，逐步向前传播，但是这只是在传播事件，并没有执行真正的网络IO
//  将AsyncSocketHandler添加到Pipeline的最前面（作为第一个Handler）,
//  当wirte事件传播到AsyncSocketHandler时，由AsyncSocketHandler完成真正的网络IO，
// AsyncSocketHandler后面添加的Handler的Rin类型和AsyncSocketHandler的Rout类型匹配，否则将导致类型转换错误
class AsyncSocketHandler
  : public wangle::BytesToBytesHandler,
    public folly::AsyncTransportWrapper::ReadCallback {
 public:
  explicit AsyncSocketHandler(
      std::shared_ptr<folly::AsyncTransportWrapper> socket) //设置网络socket进行发包 和收包
    : socket_(std::move(socket)) {}

  AsyncSocketHandler(AsyncSocketHandler&&) = default;

  ~AsyncSocketHandler() override {
    detachReadCallback();

    if (socket_) {
      auto evb = socket_->getEventBase();
      if (evb) {
        evb->runImmediatelyOrRunInEventBaseThreadAndWait(
            [s = std::move(socket_)]() mutable {
              s.reset();
            });
      }
    }
  }

  void attachReadCallback() {
    socket_->setReadCB(socket_->good() ? this : nullptr);
  }

  void detachReadCallback() {
    if (socket_ && socket_->getReadCallback() == this) {
      socket_->setReadCB(nullptr);
    }
    auto ctx = getContext();
    if (ctx && !firedInactive_) {
      firedInactive_ = true;
      ctx->fireTransportInactive();
    }
  }

  void attachEventBase(folly::EventBase* eventBase) {
    if (eventBase && !socket_->getEventBase()) {
      socket_->attachEventBase(eventBase);
    }
  }

  void detachEventBase() {
    detachReadCallback();
    if (socket_->getEventBase()) {
      socket_->detachEventBase();
    }
  }

  void transportActive(Context* ctx) override {
    ctx->getPipeline()->setTransport(socket_);
    attachReadCallback();
    firedInactive_ = false;
    ctx->fireTransportActive();
  }

  void transportInactive(Context* ctx) override {
    // detachReadCallback invokes fireTransportInactive() if the transport
    // is currently active.
      //当连接可用时，添加Read回调，也就是AsyncSocketHandler本身（作为Pipeline的第一个Handler）
    detachReadCallback();
    ctx->getPipeline()->setTransport(nullptr);
  }

  void detachPipeline(Context*) override {
    detachReadCallback();
  }

  folly::Future<folly::Unit> write(
      Context* ctx,
      std::unique_ptr<folly::IOBuf> buf) override {
      // 可写之前刷新超时时间
    refreshTimeout();
    if (UNLIKELY(!buf)) {
      return folly::makeFuture();
    }
      // 写之前判断当前的连接状态
    if (!socket_->good()) {
      VLOG(5) << "socket is closed in write()";
      return folly::makeFuture<folly::Unit>(folly::AsyncSocketException(
          folly::AsyncSocketException::AsyncSocketExceptionType::NOT_OPEN,
          "socket is closed in write()"));
    }

    auto cb = new WriteCallback();
    auto future = cb->promise_.getFuture();
      // 调用AsyncSocket执行真正的网络IO操作,
    socket_->writeChain(cb, std::move(buf), ctx->getWriteFlags());
    return future;
  }

  folly::Future<folly::Unit> writeException(Context* ctx,
                                            folly::exception_wrapper) override {
    return shutdown(ctx, true);
  }

  folly::Future<folly::Unit> close(Context* ctx) override {
    bool shutdownWriteOnly = isSet(ctx->getWriteFlags(),
                                   folly::WriteFlags::WRITE_SHUTDOWN);
    if (shutdownWriteOnly) {
      socket_->shutdownWrite();
      return folly::makeFuture();
    } else {
      return shutdown(ctx, false);
    }
  }

  // Must override to avoid warnings about hidden overloaded virtual due to
  // AsyncSocket::ReadCallback::readEOF()
  void readEOF(Context* ctx) override {
    ctx->fireReadEOF();
  }

  void getReadBuffer(void** bufReturn, size_t* lenReturn) override {
    const auto readBufferSettings = getContext()->getReadBufferSettings();
    const auto ret = bufQueue_.preallocate(
        readBufferSettings.first,
        readBufferSettings.second);
    *bufReturn = ret.first;
    *lenReturn = ret.second;
  }

//  AsyncSocketHandler本身还是folly::AsyncTransportWrapper::ReadCallback的子类，
//  因此它可以作为AsyncSocket的回调被触发，当AsyncSocket读到网络数据后，
//  会调用readDataAvailable方法（folly::AsyncTransportWrapper::ReadCallback中的接口
  void readDataAvailable(size_t len) noexcept override {
    refreshTimeout();
    bufQueue_.postallocate(len);
    // 在pipeline中引发这个事件传播
    getContext()->fireRead(bufQueue_);
  }

  void readEOF() noexcept override {
    getContext()->fireReadEOF();
  }

  void readErr(const folly::AsyncSocketException& ex)
    noexcept override {
    getContext()->fireReadException(
        folly::make_exception_wrapper<folly::AsyncSocketException>(ex));
  }

 private:
  void refreshTimeout() {
    auto manager = getContext()->getPipeline()->getPipelineManager();
    if (manager) {
      manager->refreshTimeout();
    }
  }

  folly::Future<folly::Unit> shutdown(Context* ctx, bool closeWithReset) {
    if (socket_) {
      detachReadCallback();
      if (closeWithReset) {
        socket_->closeWithReset();
      } else {
        socket_->closeNow();
      }
    }
    if (!pipelineDeleted_) {
      pipelineDeleted_ = true;
      ctx->getPipeline()->deletePipeline();
    }
    return folly::makeFuture();
  }

  class WriteCallback : private folly::AsyncTransportWrapper::WriteCallback {
    void writeSuccess() noexcept override {
      promise_.setValue();
      delete this;
    }

    void writeErr(size_t /* bytesWritten */,
                  const folly::AsyncSocketException& ex)
      noexcept override {
      promise_.setException(ex);
      delete this;
    }

   private:
    friend class AsyncSocketHandler;
    folly::Promise<folly::Unit> promise_;
  };

  folly::IOBufQueue bufQueue_{folly::IOBufQueue::cacheChainLength()};
  std::shared_ptr<folly::AsyncTransportWrapper> socket_{nullptr};  //进行网络收发
  bool firedInactive_{false};
  bool pipelineDeleted_{false};
};

} // namespace wangle
