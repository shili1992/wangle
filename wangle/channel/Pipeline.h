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

#include <boost/variant.hpp>
#include <folly/ExceptionWrapper.h>
#include <folly/Memory.h>
#include <folly/futures/Future.h>
#include <folly/Unit.h>
#include <folly/io/IOBufQueue.h>
#include <folly/io/async/AsyncTransport.h>
#include <folly/io/async/DelayedDestruction.h>
#include <wangle/acceptor/SecureTransportType.h>
#include <wangle/acceptor/TransportInfo.h>
#include <wangle/channel/HandlerContext.h>

namespace wangle {

class PipelineBase;
class Acceptor;

//如果你需要监听Pipeline的delete和refresh事件，那么可以自己实现一个PipelineManager并设置到Pipeline上。
class PipelineManager {
 public:
  virtual ~PipelineManager() = default;
  virtual void deletePipeline(PipelineBase* pipeline) = 0; // deletePipeline会在显示调用一个pipeline的close方法时被调用，一般用来完成该Pipeline相关的资源释放
  virtual void refreshTimeout() {}  // refreshTimeout主要在Pipeline发生读写事件时被回调，主要用来刷新Pipeline的空闲时间
};

class PipelineBase : public std::enable_shared_from_this<PipelineBase> {
 public:
  virtual ~PipelineBase() = default;

  void setPipelineManager(PipelineManager* manager) {
    manager_ = manager;
  }

  PipelineManager* getPipelineManager() {
    return manager_;
  }

  void deletePipeline() {
    if (manager_) {
      manager_->deletePipeline(this);
    }
  }

  void setTransport(std::shared_ptr<folly::AsyncTransport> transport) {
    transport_ = transport;
  }

  //Wangle中的Pipeline兼有Channel的功能
  // getTransport，该方法可以获得一个底层的AsyncTransport，而该AsyncTransport拥有所有的底层连接信息
  std::shared_ptr<folly::AsyncTransport> getTransport() {
    return transport_;
  }

  void setWriteFlags(folly::WriteFlags flags);
  folly::WriteFlags getWriteFlags();

  void setReadBufferSettings(uint64_t minAvailable, uint64_t allocationSize);
  std::pair<uint64_t, uint64_t> getReadBufferSettings();

  void setTransportInfo(std::shared_ptr<TransportInfo> tInfo);
  std::shared_ptr<TransportInfo> getTransportInfo();

  template <class H>
  PipelineBase& addBack(std::shared_ptr<H> handler);

  template <class H>
  PipelineBase& addBack(H&& handler);

  template <class H>
  PipelineBase& addBack(H* handler);

  template <class H>
  PipelineBase& addFront(std::shared_ptr<H> handler);

  template <class H>
  PipelineBase& addFront(H&& handler);

  template <class H>
  PipelineBase& addFront(H* handler);

  template <class H>
  PipelineBase& remove(H* handler);

  template <class H>
  PipelineBase& remove();

  PipelineBase& removeFront();

  PipelineBase& removeBack();

  template <class H>
  H* getHandler(int i);

  template <class H>
  H* getHandler();

  template <class H>
  typename ContextType<H>::type* getContext(int i);

  template <class H>
  typename ContextType<H>::type* getContext();

  // If one of the handlers owns the pipeline itself, use setOwner to ensure
  // that the pipeline doesn't try to detach the handler during destruction,
  // lest destruction ordering issues occur.
  // See thrift/lib/cpp2/async/Cpp2Channel.cpp for an example
  template <class H>
  bool setOwner(H* handler);

  virtual void finalize() = 0;

  size_t numHandlers() const;

 protected:
  template <class Context>
  void addContextFront(Context* ctx);

  void detachHandlers();

  std::vector<std::shared_ptr<PipelineContext>> ctxs_; //所有的PipelineContext，  该vector种使用的是智能指针，可以保持对Context的引用
  std::vector<PipelineContext*> inCtxs_;  //inbound 类型的PipelineContext， 这里放的是Context的指针，因为引用在上面的容器中已经保持
  std::vector<PipelineContext*> outCtxs_; //outbound 类型的PipelineContext

 private:
  PipelineManager* manager_{nullptr};
  std::shared_ptr<folly::AsyncTransport> transport_;
  std::shared_ptr<TransportInfo> transportInfo_;

  template <class Context>
  PipelineBase& addHelper(std::shared_ptr<Context>&& ctx, bool front);

  template <class H>
  PipelineBase& removeHelper(H* handler, bool checkEqual);

  typedef std::vector<std::shared_ptr<PipelineContext>>::iterator
    ContextIterator;

  ContextIterator removeAt(const ContextIterator& it);

  folly::WriteFlags writeFlags_{folly::WriteFlags::NONE};
  std::pair<uint64_t, uint64_t> readBufferSettings_{2048, 2048};

  std::shared_ptr<PipelineContext> owner_;
};

/*
 * R is the inbound type, i.e. inbound calls start with pipeline.read(R)
 * W is the outbound type, i.e. outbound calls start with pipeline.write(W)
 *
 * Use Unit for one of the types if your pipeline is unidirectional.
 * If R is Unit, read(), readEOF(), and readException() will be disabled.
 * If W is Unit, write() and close() will be disabled.
 */
// Pipeline主要定义和实现了一些和Handler对应的常用方法：read、readEOF、readException、transportActive、transportInactive、write、writeException、close。
template <class R, class W = folly::Unit>
class Pipeline : public PipelineBase {
 public:
  using Ptr = std::shared_ptr<Pipeline>;

  static Ptr create() {
    return std::shared_ptr<Pipeline>(new Pipeline());
  }

  ~Pipeline() override;

  template <class T = R>
  typename std::enable_if<!std::is_same<T, folly::Unit>::value>::type
  read(R msg); //front_->read(std::forward<R>(msg)); --> this->handler_->read(this, std::forward<Rin>(msg));

  template <class T = R>
  typename std::enable_if<!std::is_same<T, folly::Unit>::value>::type
  readEOF();  //front_->readEOF();

  template <class T = R>
  typename std::enable_if<!std::is_same<T, folly::Unit>::value>::type
  readException(folly::exception_wrapper e);  //front_->readException(std::move(e));

  template <class T = R>
  typename std::enable_if<!std::is_same<T, folly::Unit>::value>::type
  transportActive();  // front_->transportActive();

  template <class T = R>
  typename std::enable_if<!std::is_same<T, folly::Unit>::value>::type
  transportInactive();  // front_->transportActive();

    template <class T = W>
  typename std::enable_if<!std::is_same<T, folly::Unit>::value,
                          folly::Future<folly::Unit>>::type
  write(W msg);

  template <class T = W>
  typename std::enable_if<!std::is_same<T, folly::Unit>::value,
                          folly::Future<folly::Unit>>::type
  writeException(folly::exception_wrapper e);

  template <class T = W>
  typename std::enable_if<!std::is_same<T, folly::Unit>::value,
                          folly::Future<folly::Unit>>::type
  close();

  void finalize() override;

 protected:
  Pipeline();
  explicit Pipeline(bool isStatic);

 private:
  bool isStatic_{false};

  InboundLink<R>* front_{nullptr};
  OutboundLink<W>* back_{nullptr};
};

} // namespace wangle

namespace folly {

class AsyncSocket;
class AsyncTransportWrapper;
class AsyncUDPSocket;

}

namespace wangle {

using DefaultPipeline =
    Pipeline<folly::IOBufQueue&, std::unique_ptr<folly::IOBuf>>;

template <typename Pipeline>
class PipelineFactory {
 public:
  virtual typename Pipeline::Ptr newPipeline(
      std::shared_ptr<folly::AsyncTransportWrapper>) = 0;

  virtual typename Pipeline::Ptr newPipeline(
      std::shared_ptr<folly::AsyncUDPSocket> /* serverSocket */,
      const folly::SocketAddress& /* clientAddr */) {
    return nullptr;
  }

  virtual ~PipelineFactory() = default;
};

struct ConnInfo {
  folly::AsyncTransportWrapper* sock;
  const folly::SocketAddress* clientAddr;
  const std::string& nextProtoName;
  SecureTransportType secureType;
  const TransportInfo& tinfo;
};

enum class ConnEvent {
  CONN_ADDED,
  CONN_REMOVED,
};

typedef boost::variant<folly::IOBuf*,
                       folly::AsyncTransportWrapper*,
                       ConnInfo&,
                       ConnEvent,
                       std::tuple<folly::IOBuf*,
                                  std::shared_ptr<folly::AsyncUDPSocket>,
                                  folly::SocketAddress>> AcceptPipelineType;
typedef Pipeline<AcceptPipelineType> AcceptPipeline;

class AcceptPipelineFactory {
 public:
  virtual typename AcceptPipeline::Ptr newPipeline(Acceptor* acceptor) = 0;

  virtual ~AcceptPipelineFactory() = default;
};

}

#include <wangle/channel/Pipeline-inl.h>
