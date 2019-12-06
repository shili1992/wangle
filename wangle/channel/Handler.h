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
#include <wangle/channel/Pipeline.h>
#include <folly/io/IOBuf.h>
#include <folly/io/IOBufQueue.h>

namespace wangle {

template <class Context>
class HandlerBase {
 public:
  virtual ~HandlerBase() = default;

  virtual void attachPipeline(Context* /*ctx*/) {}
  virtual void detachPipeline(Context* /*ctx*/) {}

    // 获取绑定的Context
  Context* getContext() {
    if (attachCount_ != 1) {
      return nullptr;
    }
    CHECK(ctx_);
    return ctx_;
  }

 private:
  friend PipelineContext;
  uint64_t attachCount_{0};  // 绑定计数，同一个handler可以被同时绑定到不同的pipeline中
  Context* ctx_{nullptr};  //该Handler绑定的Context
};


    //它具有四个模板参数： Rin、Rout、Win、Wout，其中Rin作为Handler和Context中read方法中消息的数据类型，
    // Rout是作为Context中fireRead方法的参数类型。同理，Win是作为Handler和Context中wirte方法的消息参数类型，
    //而Wout是作为Context中fireWrite的消息参数类型。可以这么理解：Xout是作为以fire开头的事件方法的参数类型。
template <class Rin, class Rout = Rin, class Win = Rout, class Wout = Rin>
class Handler : public HandlerBase<HandlerContext<Rout, Wout>> {
 public:
  static const HandlerDir dir = HandlerDir::BOTH;   // 方向为双向

  typedef Rin rin;
  typedef Rout rout;
  typedef Win win;
  typedef Wout wout;
  typedef HandlerContext<Rout, Wout> Context;  // 声明该HandlerContext类型
  ~Handler() override = default;

    //Handler也相应的定义了inbound类型和outbound类型事件，分别对应方法：read、readEOF、readException、transportActive、
    // transportInactive、write、writeException、close（这些方法和Pipeline中一一对应）
    //除了read和write两个方法是纯虚接口之外，其他的方法都提供了默认实现：就是将事件进行透传（调用Context里fireXxx方法）

  virtual void read(Context* ctx, Rin msg) = 0;
  virtual void readEOF(Context* ctx) {
    ctx->fireReadEOF();
  }
  virtual void readException(Context* ctx, folly::exception_wrapper e) {
    ctx->fireReadException(std::move(e));
  }
  virtual void transportActive(Context* ctx) {
    ctx->fireTransportActive();
  }
  virtual void transportInactive(Context* ctx) {
    ctx->fireTransportInactive();
  }

  virtual folly::Future<folly::Unit> write(Context* ctx, Win msg) = 0;
  virtual folly::Future<folly::Unit> writeException(Context* ctx,
                                                   folly::exception_wrapper e) {
    return ctx->fireWriteException(std::move(e));
  }
  virtual folly::Future<folly::Unit> close(Context* ctx) {
    return ctx->fireClose();
  }

  /*
  // Other sorts of things we might want, all shamelessly stolen from Netty
  // inbound
  virtual void exceptionCaught(
      HandlerContext* ctx,
      folly::exception_wrapper e) {}
  virtual void channelRegistered(HandlerContext* ctx) {}
  virtual void channelUnregistered(HandlerContext* ctx) {}
  virtual void channelReadComplete(HandlerContext* ctx) {}
  virtual void userEventTriggered(HandlerContext* ctx, void* evt) {}
  virtual void channelWritabilityChanged(HandlerContext* ctx) {}

  // outbound
  virtual folly::Future<folly::Unit> bind(
      HandlerContext* ctx,
      SocketAddress localAddress) {}
  virtual folly::Future<folly::Unit> connect(
          HandlerContext* ctx,
          SocketAddress remoteAddress, SocketAddress localAddress) {}
  virtual folly::Future<folly::Unit> disconnect(HandlerContext* ctx) {}
  virtual folly::Future<folly::Unit> deregister(HandlerContext* ctx) {}
  virtual folly::Future<folly::Unit> read(HandlerContext* ctx) {}
  virtual void flush(HandlerContext* ctx) {}
  */
};

// inbound类型的Handler （默认情况下读入和读出的类型是一致）， 并未继承自Handler， 并不需要实现write函数
template <class Rin, class Rout = Rin>
class InboundHandler : public HandlerBase<InboundHandlerContext<Rout>> {
 public:
  static const HandlerDir dir = HandlerDir::IN;

  typedef Rin rin;
  typedef Rout rout;
  typedef folly::Unit win;
  typedef folly::Unit wout;
  typedef InboundHandlerContext<Rout> Context;
  ~InboundHandler() override = default;

  virtual void read(Context* ctx, Rin msg) = 0;
    // 下面的默认实现都是事件的透传
  virtual void readEOF(Context* ctx) {
    ctx->fireReadEOF();
  }
  virtual void readException(Context* ctx, folly::exception_wrapper e) {
    ctx->fireReadException(std::move(e));
  }
  virtual void transportActive(Context* ctx) {
    ctx->fireTransportActive();
  }
  virtual void transportInactive(Context* ctx) {
    ctx->fireTransportInactive();
  }
};

template <class Win, class Wout = Win>
class OutboundHandler : public HandlerBase<OutboundHandlerContext<Wout>> {
 public:
  static const HandlerDir dir = HandlerDir::OUT;

  typedef folly::Unit rin;
  typedef folly::Unit rout;
  typedef Win win;
  typedef Wout wout;
  typedef OutboundHandlerContext<Wout> Context;
  ~OutboundHandler() override = default;

  virtual folly::Future<folly::Unit> write(Context* ctx, Win msg) = 0;
  virtual folly::Future<folly::Unit> writeException(
      Context* ctx, folly::exception_wrapper e) {
    return ctx->fireWriteException(std::move(e));
  }
  virtual folly::Future<folly::Unit> close(Context* ctx) {
    return ctx->fireClose();
  }
};

//Handler所有的事件方法中只有read和write是纯虚接口，这样用户每次实现自己的Handler时都需要override这两个方法（即使只是完成简单的事件透传），
// 因此，为了方便用户编写自己的Handler，Wangle提供了HandlerAdapter，HandlerAdapter其实很简单，
// 就是以事件透传的方式重写（override）了read个write两个方法。
template <class R, class W = R>
class HandlerAdapter : public Handler<R, R, W, W> {
 public:
  typedef typename Handler<R, R, W, W>::Context Context;

    // 将read事件直接进行透传
  void read(Context* ctx, R msg) override {
    ctx->fireRead(std::forward<R>(msg));
  }
    // 将write事件直接进行透传
  folly::Future<folly::Unit> write(Context* ctx, W msg) override {
    return ctx->fireWrite(std::forward<W>(msg));
  }
};

typedef HandlerAdapter<folly::IOBufQueue&, std::unique_ptr<folly::IOBuf>>
BytesToBytesHandler;

typedef InboundHandler<folly::IOBufQueue&, std::unique_ptr<folly::IOBuf>>
InboundBytesToBytesHandler;

typedef OutboundHandler<std::unique_ptr<folly::IOBuf>>
OutboundBytesToBytesHandler;

} // namespace wangle
