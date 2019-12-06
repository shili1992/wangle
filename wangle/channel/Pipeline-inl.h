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

#include <glog/logging.h>

namespace wangle {

template <class R, class W>
Pipeline<R, W>::Pipeline() : isStatic_(false) {}

template <class R, class W>
Pipeline<R, W>::Pipeline(bool isStatic) : isStatic_(isStatic) {
  CHECK(isStatic_);
}

template <class R, class W>
Pipeline<R, W>::~Pipeline() {
  if (!isStatic_) {
    detachHandlers();
  }
}

// 首先，会根据要添加的handler类型定义一个Context（Context可以看成是Handler的外套)类型，
// 然后根据这个Context类型创建一个Context：参数为Pipeline指针和handler，最终addHelper会将Context添加到容器管理起来
template <class H>
PipelineBase& PipelineBase::addBack(std::shared_ptr<H> handler) {
  typedef typename ContextType<H>::type Context; // 声明Conetxt类型,ContextImpl<Handler>、InboundContextImpl<Handler>、OutboundContextImpl<Handler>其中之一
    // 使用Context包装Handler后，将其添加到pipeline中，Context中还持有pipeline的引用
  return addHelper(
      std::make_shared<Context>(shared_from_this(), std::move(handler)),
      false); // false标识添加到尾部
}

template <class H>
PipelineBase& PipelineBase::addBack(H&& handler) {
  return addBack(std::make_shared<H>(std::forward<H>(handler)));
}

template <class H>
PipelineBase& PipelineBase::addBack(H* handler) {
  return addBack(std::shared_ptr<H>(handler, [](H*){}));
}

template <class H>
PipelineBase& PipelineBase::addFront(std::shared_ptr<H> handler) {
  typedef typename ContextType<H>::type Context;
  return addHelper(
      std::make_shared<Context>(shared_from_this(), std::move(handler)),
      true);
}

template <class H>
PipelineBase& PipelineBase::addFront(H&& handler) {
  return addFront(std::make_shared<H>(std::forward<H>(handler)));
}

template <class H>
PipelineBase& PipelineBase::addFront(H* handler) {
  return addFront(std::shared_ptr<H>(handler, [](H*){}));
}

template <class H>
PipelineBase& PipelineBase::removeHelper(H* handler, bool checkEqual) {
  typedef typename ContextType<H>::type Context;
  bool removed = false;

  auto it = ctxs_.begin();
  while (it != ctxs_.end()) {
    auto ctx = std::dynamic_pointer_cast<Context>(*it);
    if (ctx && (!checkEqual || ctx->getHandler() == handler)) {
      it = removeAt(it);
      removed = true;
    } else {
      it++;
    }
  }

  if (!removed) {
    throw std::invalid_argument("No such handler in pipeline");
  }

  return *this;
}

template <class H>
PipelineBase& PipelineBase::remove() {
  return removeHelper<H>(nullptr, false);
}

template <class H>
PipelineBase& PipelineBase::remove(H* handler) {
  return removeHelper<H>(handler, true);
}

template <class H>
H* PipelineBase::getHandler(int i) {
  return getContext<H>(i)->getHandler();
}

template <class H>
H* PipelineBase::getHandler() {
  auto ctx = getContext<H>();
  return ctx ? ctx->getHandler() : nullptr;
}

template <class H>
typename ContextType<H>::type* PipelineBase::getContext(int i) {
  auto ctx = dynamic_cast<typename ContextType<H>::type*>(ctxs_[i].get());
  CHECK(ctx);
  return ctx;
}

template <class H>
typename ContextType<H>::type* PipelineBase::getContext() {
  for (auto pipelineCtx : ctxs_) {
    auto ctx = dynamic_cast<typename ContextType<H>::type*>(pipelineCtx.get());
    if (ctx) {
      return ctx;
    }
  }
  return nullptr;
}

template <class H>
bool PipelineBase::setOwner(H* handler) {
  typedef typename ContextType<H>::type Context;
  for (auto& ctx : ctxs_) {
    auto ctxImpl = dynamic_cast<Context*>(ctx.get());
    if (ctxImpl && ctxImpl->getHandler() == handler) {
      owner_ = ctx;
      return true;
    }
  }
  return false;
}

template <class Context>
void PipelineBase::addContextFront(Context* ctx) {
  addHelper(std::shared_ptr<Context>(ctx, [](Context*){}), true);
}

template <class Context>
PipelineBase& PipelineBase::addHelper(
    std::shared_ptr<Context>&& ctx, /* Context内部包含了Pipeline、Handler*/
    bool front) {
  ctxs_.insert(front ? ctxs_.begin() : ctxs_.end(), ctx);
    // 然后根据方向（BOTH、IN、OUT分别加入相应的vector中）
  if (Context::dir == HandlerDir::BOTH || Context::dir == HandlerDir::IN) {
    inCtxs_.insert(front ? inCtxs_.begin() : inCtxs_.end(), ctx.get());
  }
  if (Context::dir == HandlerDir::BOTH || Context::dir == HandlerDir::OUT) {
    outCtxs_.insert(front ? outCtxs_.begin() : outCtxs_.end(), ctx.get());
  }
  return *this;
}

namespace detail {

template <class T>
inline void logWarningIfNotUnit(const std::string& warning) {
  LOG(WARNING) << warning;
}

template <>
inline void logWarningIfNotUnit<folly::Unit>(const std::string& /*warning*/) {
  // do nothing
}

} // detail

template <class R, class W>
template <class T>
typename std::enable_if<!std::is_same<T, folly::Unit>::value>::type
Pipeline<R, W>::read(R msg) {
  if (!front_) {
    throw std::invalid_argument("read(): no inbound handler in Pipeline");
  }
  front_->read(std::forward<R>(msg));
}

template <class R, class W>
template <class T>
typename std::enable_if<!std::is_same<T, folly::Unit>::value>::type
Pipeline<R, W>::readEOF() {
  if (!front_) {
    throw std::invalid_argument("readEOF(): no inbound handler in Pipeline");
  }
  front_->readEOF();
}

template <class R, class W>
template <class T>
typename std::enable_if<!std::is_same<T, folly::Unit>::value>::type
Pipeline<R, W>::transportActive() {
  if (front_) {
    front_->transportActive();
  }
}

template <class R, class W>
template <class T>
typename std::enable_if<!std::is_same<T, folly::Unit>::value>::type
Pipeline<R, W>::transportInactive() {
  if (front_) {
    front_->transportInactive();
  }
}

template <class R, class W>
template <class T>
typename std::enable_if<!std::is_same<T, folly::Unit>::value>::type
Pipeline<R, W>::readException(folly::exception_wrapper e) {
  if (!front_) {
    throw std::invalid_argument(
        "readException(): no inbound handler in Pipeline");
  }
  front_->readException(std::move(e));
}

template <class R, class W>
template <class T>
typename std::enable_if<!std::is_same<T, folly::Unit>::value,
                        folly::Future<folly::Unit>>::type
Pipeline<R, W>::write(W msg) {
  if (!back_) {
    throw std::invalid_argument("write(): no outbound handler in Pipeline");
  }
  return back_->write(std::forward<W>(msg));
}

template <class R, class W>
template <class T>
typename std::enable_if<!std::is_same<T, folly::Unit>::value,
                        folly::Future<folly::Unit>>::type
  Pipeline<R, W>::writeException(folly::exception_wrapper e) {
  if (!back_) {
    throw std::invalid_argument(
      "writeException(): no outbound handler in Pipeline");
  }
  return back_->writeException(std::move(e));
}

template <class R, class W>
template <class T>
typename std::enable_if<!std::is_same<T, folly::Unit>::value,
                        folly::Future<folly::Unit>>::type
Pipeline<R, W>::close() {
  if (!back_) {
    throw std::invalid_argument("close(): no outbound handler in Pipeline");
  }
  return back_->close();
}

// TODO Have read/write/etc check that pipeline has been finalized
//    ContextImpl就是最终的Context实现，也就是要被添加到Pipeline中（比如使用addBack）的容器
//    （ctxs_，inCtxs_，outCtxs_）的最终Context，
//    在最后的finalize方法中还会进一步将容器中的Context组装成front_和back_单向链表。
template <class R, class W>
void Pipeline<R, W>::finalize() {
  front_ = nullptr;
  if (!inCtxs_.empty()) {
      //遍历inCtxs_容器，对容器中的每一个Context调用其setNextIn方法将Context组成一个单向链表front_
    front_ = dynamic_cast<InboundLink<R>*>(inCtxs_.front());
    for (size_t i = 0; i < inCtxs_.size() - 1; i++) {
      inCtxs_[i]->setNextIn(inCtxs_[i+1]);
    }
    inCtxs_.back()->setNextIn(nullptr);
  }

  // 返回结果是从后指向前面 back指向最后一个元素
  back_ = nullptr;
  if (!outCtxs_.empty()) {
    back_ = dynamic_cast<OutboundLink<W>*>(outCtxs_.back());
    for (size_t i = outCtxs_.size() - 1; i > 0; i--) {
      outCtxs_[i]->setNextOut(outCtxs_[i-1]);
    }
    outCtxs_.front()->setNextOut(nullptr);
  }

  if (!front_) {
    detail::logWarningIfNotUnit<R>(
        "No inbound handler in Pipeline, inbound operations will throw "
        "std::invalid_argument");
  }
  if (!back_) {
    detail::logWarningIfNotUnit<W>(
        "No outbound handler in Pipeline, outbound operations will throw "
        "std::invalid_argument");
  }

    //遍历Context的总容器ctxs_，为每一个Context调用attachPipeline方法，该方法主要工作就是把Context绑定到对应的Handler上
    //   handler中添加了绑定到 context, Context和Handler都互相持有对方的引用
  for (auto it = ctxs_.rbegin(); it != ctxs_.rend(); it++) {
    (*it)->attachPipeline();
  }
}

} // namespace wangle
