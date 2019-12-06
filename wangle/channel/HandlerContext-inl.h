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

#include <folly/Format.h>

namespace wangle {

class PipelineContext {
 public:
  virtual ~PipelineContext() = default;

    // 依附到一个pipeline中
  virtual void attachPipeline() = 0;
    // 从pipeline中分离
  virtual void detachPipeline() = 0;

    // 将一个HandlerContext绑定到handler上
  template <class H, class HandlerContext>
  void attachContext(H* handler, HandlerContext* ctx) {
        // 只有第一次绑定的时候才会设置
    if (++handler->attachCount_ == 1) {
      handler->ctx_ = ctx;
    } else {
      handler->ctx_ = nullptr;
    }
  }

  template <class H, class HandlerContext>
  void detachContext(H* handler, HandlerContext* /*ctx*/) {
    if (handler->attachCount_ >= 1) {
      --handler->attachCount_;
    }
    handler->ctx_ = nullptr;
  }
    // 设置下一个inbound类型的Context
    virtual void setNextIn(PipelineContext * ctx) = 0;
    // 设置下一个outbound类型的Context
    virtual void setNextOut(PipelineContext * ctx) = 0;
    // 获取方向(Context方向依赖于Handler方向)
    virtual HandlerDir getDirection() = 0;
};

// InboundLink只是把Pipeline主要方法中的IN方向单独抽象出来，都是一个IN事件（输入事件）
template <class In>
class InboundLink {
 public:
  virtual ~InboundLink() = default;
  virtual void read(In msg) = 0;
  virtual void readEOF() = 0;
  virtual void readException(folly::exception_wrapper e) = 0;
  virtual void transportActive() = 0;
  virtual void transportInactive() = 0;
};

// OutboundLink定义的都是OUT事件类型的操作。
template <class Out>
class OutboundLink {
 public:
  virtual ~OutboundLink() = default;
  virtual folly::Future<folly::Unit> write(Out msg) = 0;
  virtual folly::Future<folly::Unit> writeException(
      folly::exception_wrapper e) = 0;
  virtual folly::Future<folly::Unit> close() = 0;
};

// ContextImplBase主要实现了PipelineContext接口方法
template <class H, class Context>
class ContextImplBase : public PipelineContext {
 public:
  ~ContextImplBase() override = default;

    // 获取Context绑定的Handler
  H* getHandler() {
    return handler_.get();
  }
    // Context初始化，参数为Context所属的Pipeline weak_ptr，Context要绑定的Handler  shared_ptr
  void initialize(
      std::weak_ptr<PipelineBase> pipeline,
      std::shared_ptr<H> handler) {
    pipelineWeak_ = pipeline;
    pipelineRaw_ = pipeline.lock().get();
    handler_ = std::move(handler);
  }

  // PipelineContext overrides
  void attachPipeline() override {
    if (!attached_) {
        //handler中添加了绑定到 context, Context和Handler都互相持有对方的引用
      this->attachContext(handler_.get(), impl_);
      handler_->attachPipeline(impl_);
      attached_ = true;
    }
  }
    // 从pipeline中分离
  void detachPipeline() override {
    handler_->detachPipeline(impl_);
    attached_ = false;
    this->detachContext(handler_.get(), impl_);
  }

  void setNextIn(PipelineContext* ctx) override {
    if (!ctx) {
      nextIn_ = nullptr;
      return;
    }
      // 转成InboundLink，因为Context是InboundLink子类
    auto nextIn = dynamic_cast<InboundLink<typename H::rout>*>(ctx);
    if (nextIn) {
      nextIn_ = nextIn;
    } else {
      throw std::invalid_argument(folly::sformat(
          "inbound type mismatch after {}", folly::demangle(typeid(H))));
    }
  }

  void setNextOut(PipelineContext* ctx) override {
    if (!ctx) {
      nextOut_ = nullptr;
      return;
    }
    auto nextOut = dynamic_cast<OutboundLink<typename H::wout>*>(ctx);
    if (nextOut) {
      nextOut_ = nextOut;
    } else {
      throw std::invalid_argument(folly::sformat(
          "outbound type mismatch after {}", folly::demangle(typeid(H))));
    }
  }
    // 获取Context的方向
  HandlerDir getDirection() override {
    return H::dir;
  }

 protected:
  Context* impl_;     // 具体的Context实现
  std::weak_ptr<PipelineBase> pipelineWeak_;  // Context中持有的Pipeline是一个weak类型的指针
  PipelineBase* pipelineRaw_;           // 该Context绑定的 pipeline的指针
  std::shared_ptr<H> handler_;         // 该Context包含的Handler
  InboundLink<typename H::rout>* nextIn_{nullptr};  //指向下一个item， 构成一个链表
  OutboundLink<typename H::wout>* nextOut_{nullptr}; // nextIn_和nextOut_就是链表的指针，用来串联起整个Context

 private:
  bool attached_{false};
};

    //HandlerContext中主要定义了以fire开头的事件传递方法；
    //InboundLink和OutboundLink分别定义了Handler中Inbound和Outbound类型的方法接口
    //ContextImplBase主要提供了Pipeline中Context在组装链表时的接口，比如：setNextIn、setNextOut，
    // 以及用于将Context绑定到handler上的attachPipeline方法。
    // ContextImpl就是最终的Context实现，也就是要被添加到Pipeline中（比如使用addBack）的容器（ctxs_，inCtxs_，outCtxs_）的最终Context，
    // 在最后的finalize方法中还会进一步将容器中的Context组装成front_和back_单向链表
template <class H>
class ContextImpl
  : public HandlerContext<typename H::rout,
                          typename H::wout>,
    public InboundLink<typename H::rin>,
    public OutboundLink<typename H::win>,
    public ContextImplBase<H, HandlerContext<typename H::rout,
                                             typename H::wout>> {
 public:
  typedef typename H::rin Rin;
  typedef typename H::rout Rout;
  typedef typename H::win Win;
  typedef typename H::wout Wout;
  static const HandlerDir dir = HandlerDir::BOTH;

  explicit ContextImpl(
      std::weak_ptr<PipelineBase> pipeline,
      std::shared_ptr<H> handler) {
    this->impl_ = this;
    this->initialize(pipeline, std::move(handler));
  }

  // For StaticPipeline
  ContextImpl() {
    this->impl_ = this;
  }

  ~ContextImpl() override = default;

  // HandlerContext overrides
  void fireRead(Rout msg) override {
    auto guard = this->pipelineWeak_.lock();  //  尝试lock，保证在事件传播阶段这个Pipeline不会销毁
    if (this->nextIn_) {
    //  将事件继续向下传播（传给下一个Inbound类型的Context）
    //  注意：这里调用的是下一个Contex的read而不是fireRead
    //  即调用下一个Context里面的Handler方法
      this->nextIn_->read(std::forward<Rout>(msg));  //调用下一个IN类型的Context的read方法
    } else {
      LOG(WARNING) << "read reached end of pipeline";
    }
  }

  void fireReadEOF() override {
    auto guard = this->pipelineWeak_.lock();
    if (this->nextIn_) {
      this->nextIn_->readEOF();
    } else {
      LOG(WARNING) << "readEOF reached end of pipeline";
    }
  }

  void fireReadException(folly::exception_wrapper e) override {
    auto guard = this->pipelineWeak_.lock();
    if (this->nextIn_) {
      this->nextIn_->readException(std::move(e));
    } else {
      LOG(WARNING) << "readException reached end of pipeline";
    }
  }

  void fireTransportActive() override {
    auto guard = this->pipelineWeak_.lock();
    if (this->nextIn_) {
      this->nextIn_->transportActive();
    }
  }

  void fireTransportInactive() override {
    auto guard = this->pipelineWeak_.lock();
    if (this->nextIn_) {
      this->nextIn_->transportInactive();
    }
  }

  //Outbound类型的事件传播
  folly::Future<folly::Unit> fireWrite(Wout msg) override {
    auto guard = this->pipelineWeak_.lock();
    if (this->nextOut_) {   // 如果还没有到最后
      return this->nextOut_->write(std::forward<Wout>(msg));
    } else {
      LOG(WARNING) << "write reached end of pipeline";
      // 如果到了最后，返回一个future
      return folly::makeFuture();
    }
  }

  folly::Future<folly::Unit> fireWriteException(
      folly::exception_wrapper e) override {
    auto guard = this->pipelineWeak_.lock();
    if (this->nextOut_) {
      return this->nextOut_->writeException(std::move(e));
    } else {
      LOG(WARNING) << "close reached end of pipeline";
      return folly::makeFuture();
    }
  }

  folly::Future<folly::Unit> fireClose() override {
    auto guard = this->pipelineWeak_.lock();
    if (this->nextOut_) {
      return this->nextOut_->close();
    } else {
      LOG(WARNING) << "close reached end of pipeline";
      return folly::makeFuture();
    }
  }

  PipelineBase* getPipeline() override {
    return this->pipelineRaw_;
  }

  std::shared_ptr<PipelineBase> getPipelineShared() override {
    return this->pipelineWeak_.lock();
  }

  void setWriteFlags(folly::WriteFlags flags) override {
    this->pipelineRaw_->setWriteFlags(flags);
  }

  folly::WriteFlags getWriteFlags() override {
    return this->pipelineRaw_->getWriteFlags();
  }

  void setReadBufferSettings(
      uint64_t minAvailable,
      uint64_t allocationSize) override {
    this->pipelineRaw_->setReadBufferSettings(minAvailable, allocationSize);
  }

  std::pair<uint64_t, uint64_t> getReadBufferSettings() override {
    return this->pipelineRaw_->getReadBufferSettings();
  }

  // InboundLink overrides
  void read(Rin msg) override {
    auto guard = this->pipelineWeak_.lock();
    this->handler_->read(this, std::forward<Rin>(msg));
  }

  void readEOF() override {
    auto guard = this->pipelineWeak_.lock();
    this->handler_->readEOF(this);
  }

  void readException(folly::exception_wrapper e) override {
    auto guard = this->pipelineWeak_.lock();
    this->handler_->readException(this, std::move(e));
  }

  void transportActive() override {
    auto guard = this->pipelineWeak_.lock();
    this->handler_->transportActive(this);
  }

  void transportInactive() override {
    auto guard = this->pipelineWeak_.lock();
    this->handler_->transportInactive(this);
  }

  // OutboundLink overrides
  folly::Future<folly::Unit> write(Win msg) override {
    auto guard = this->pipelineWeak_.lock();
    return this->handler_->write(this, std::forward<Win>(msg));
  }

  folly::Future<folly::Unit> writeException(
      folly::exception_wrapper e) override {
    auto guard = this->pipelineWeak_.lock();
    return this->handler_->writeException(this, std::move(e));
  }

  folly::Future<folly::Unit> close() override {
    auto guard = this->pipelineWeak_.lock();
    return this->handler_->close(this);
  }
};

// // inbound 类型的InboundHandlerContext
template <class H>
class InboundContextImpl
  : public InboundHandlerContext<typename H::rout>,
    public InboundLink<typename H::rin>,
    public ContextImplBase<H, InboundHandlerContext<typename H::rout>> {
 public:
  typedef typename H::rin Rin;
  typedef typename H::rout Rout;
  typedef typename H::win Win;
  typedef typename H::wout Wout;
  static const HandlerDir dir = HandlerDir::IN;

  explicit InboundContextImpl(
      std::weak_ptr<PipelineBase> pipeline,
      std::shared_ptr<H> handler) {
    this->impl_ = this;
    this->initialize(pipeline, std::move(handler));
  }

  // For StaticPipeline
  InboundContextImpl() {
    this->impl_ = this;
  }

  ~InboundContextImpl() override = default;

  // InboundHandlerContext overrides
  void fireRead(Rout msg) override {
    auto guard = this->pipelineWeak_.lock();
    if (this->nextIn_) {
      this->nextIn_->read(std::forward<Rout>(msg));
    } else {
      LOG(WARNING) << "read reached end of pipeline";
    }
  }

  void fireReadEOF() override {
    auto guard = this->pipelineWeak_.lock();
    if (this->nextIn_) {
      this->nextIn_->readEOF();
    } else {
      LOG(WARNING) << "readEOF reached end of pipeline";
    }
  }

  void fireReadException(folly::exception_wrapper e) override {
    auto guard = this->pipelineWeak_.lock();
    if (this->nextIn_) {
      this->nextIn_->readException(std::move(e));
    } else {
      LOG(WARNING) << "readException reached end of pipeline";
    }
  }

  void fireTransportActive() override {
    auto guard = this->pipelineWeak_.lock();
    if (this->nextIn_) {
      this->nextIn_->transportActive();
    }
  }

  void fireTransportInactive() override {
    auto guard = this->pipelineWeak_.lock();
    if (this->nextIn_) {
      this->nextIn_->transportInactive();
    }
  }

  PipelineBase* getPipeline() override {
    return this->pipelineRaw_;
  }

  std::shared_ptr<PipelineBase> getPipelineShared() override {
    return this->pipelineWeak_.lock();
  }

  // InboundLink overrides
  void read(Rin msg) override {
    auto guard = this->pipelineWeak_.lock();
    this->handler_->read(this, std::forward<Rin>(msg));
  }

  void readEOF() override {
    auto guard = this->pipelineWeak_.lock();
    this->handler_->readEOF(this);
  }

  void readException(folly::exception_wrapper e) override {
    auto guard = this->pipelineWeak_.lock();
    this->handler_->readException(this, std::move(e));
  }

  void transportActive() override {
    auto guard = this->pipelineWeak_.lock();
    this->handler_->transportActive(this);
  }

  void transportInactive() override {
    auto guard = this->pipelineWeak_.lock();
    this->handler_->transportInactive(this);
  }
};

template <class H>
class OutboundContextImpl
  : public OutboundHandlerContext<typename H::wout>,
    public OutboundLink<typename H::win>,
    public ContextImplBase<H, OutboundHandlerContext<typename H::wout>> {
 public:
  typedef typename H::rin Rin;
  typedef typename H::rout Rout;
  typedef typename H::win Win;
  typedef typename H::wout Wout;
  static const HandlerDir dir = HandlerDir::OUT;

  explicit OutboundContextImpl(
      std::weak_ptr<PipelineBase> pipeline,
      std::shared_ptr<H> handler) {
    this->impl_ = this;
    this->initialize(pipeline, std::move(handler));
  }

  // For StaticPipeline
  OutboundContextImpl() {
    this->impl_ = this;
  }

  ~OutboundContextImpl() override = default;

  // OutboundHandlerContext overrides
  folly::Future<folly::Unit> fireWrite(Wout msg) override {
    auto guard = this->pipelineWeak_.lock();
    if (this->nextOut_) {
      return this->nextOut_->write(std::forward<Wout>(msg));
    } else {
      LOG(WARNING) << "write reached end of pipeline";
      return folly::makeFuture();
    }
  }

  folly::Future<folly::Unit> fireWriteException(
      folly::exception_wrapper e) override {
    auto guard = this->pipelineWeak_.lock();
    if (this->nextOut_) {
      return this->nextOut_->writeException(std::move(e));
    } else {
      LOG(WARNING) << "close reached end of pipeline";
      return folly::makeFuture();
    }
  }

  folly::Future<folly::Unit> fireClose() override {
    auto guard = this->pipelineWeak_.lock();
    if (this->nextOut_) {
      return this->nextOut_->close();
    } else {
      LOG(WARNING) << "close reached end of pipeline";
      return folly::makeFuture();
    }
  }

  PipelineBase* getPipeline() override {
    return this->pipelineRaw_;
  }

  std::shared_ptr<PipelineBase> getPipelineShared() override {
    return this->pipelineWeak_.lock();
  }

  // OutboundLink overrides
  // Pipeline的write方法只是简单的调用back_的wirte方法，也就是OUT类型的事件会从Pipeline的最后一个Context依次向前传递(只传递给OUT类型的handler)。
  folly::Future<folly::Unit> write(Win msg) override {
    auto guard = this->pipelineWeak_.lock();
    return this->handler_->write(this, std::forward<Win>(msg));
  }

  folly::Future<folly::Unit> writeException(
      folly::exception_wrapper e) override {
    auto guard = this->pipelineWeak_.lock();
    return this->handler_->writeException(this, std::move(e));
  }

  folly::Future<folly::Unit> close() override {
    auto guard = this->pipelineWeak_.lock();
    return this->handler_->close(this);
  }
};

//它会根据Handler的类型（具体来说是方向）决定Context的类型，
// 如果Handler是双向的，那么Context类型为ContextImpl<Handler>，
// 如果Handler的方向为IN，那么Context类型为InboundContextImpl<Handler>，
// 如果Handler的方向为OUT，那么Context类型为OutboundContextImpl<Handler>。
template <class Handler>
struct ContextType {
  typedef typename std::conditional<
    Handler::dir == HandlerDir::BOTH,
    ContextImpl<Handler>,
    typename std::conditional<
      Handler::dir == HandlerDir::IN,
      InboundContextImpl<Handler>,
      OutboundContextImpl<Handler>
    >::type>::type
  type;
};

} // namespace wangle
