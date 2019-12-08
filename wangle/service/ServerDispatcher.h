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
#include <wangle/service/Service.h>

namespace wangle {

/**
 * Dispatch requests from pipeline one at a time synchronously.
 * Concurrent requests are queued in the pipeline.
 */
 // 和 SerialClientDispatcher 对应， 只有一个
template <typename Req, typename Resp = Req>
class SerialServerDispatcher : public HandlerAdapter<Req, Resp> {
 public:

  typedef typename HandlerAdapter<Req, Resp>::Context Context;

  explicit SerialServerDispatcher(Service<Req, Resp>* service)
      : service_(service) {}

  void read(Context* ctx, Req in) override {
    auto resp = (*service_)(std::move(in)).get();  //同步等待
    ctx->fireWrite(std::move(resp)); // 写回响应
  }

 private:

  Service<Req, Resp>* service_;
};

/**
 * Dispatch requests from pipeline as they come in.
 * Responses are queued until they can be sent in order.
 */
 // 队列中的response 将按照顺序返回， 它会为每个请求附加一个请求Id，Id是自增的，这样可以异步的按照顺序发送响应了。
template <typename Req, typename Resp = Req>
class PipelinedServerDispatcher : public HandlerAdapter<Req, Resp> {
 public:

  typedef typename HandlerAdapter<Req, Resp>::Context Context;

  explicit PipelinedServerDispatcher(Service<Req, Resp>* service)
      : service_(service) {}

      //处理网络中读取请求， 并且注册回到函数返回
  void read(Context*, Req in) override {
    auto requestId = requestId_++;
    (*service_)(std::move(in)).then([requestId,this](Resp& resp){ //注册请求完之后的回调函数
      responses_[requestId] = resp; // 加入映射
      sendResponses();  // 发送响应
    });
  }

  //将所有前面response 已经就绪的response 回复
  void sendResponses() {
    auto search = responses_.find(lastWrittenId_+1);
    while (search != responses_.end()) {
      Resp resp = std::move(search->second);
      responses_.erase(search->first);
        // 响应网络传输
      this->getContext()->fireWrite(std::move(resp));
      lastWrittenId_++;
        // 是否还有响应没有发送
      search = responses_.find(lastWrittenId_+1);
    }
  }

 private:
  Service<Req, Resp>* service_;
  uint32_t requestId_{1};  //  请求id
  std::unordered_map<uint32_t, Resp> responses_;  // 请求id和响应的映射
  uint32_t lastWrittenId_{0}; // 上一次响应的请求id
};

/**
 * Dispatch requests from pipeline as they come in.  Concurrent
 * requests are assumed to have sequence id's that are taken care of
 * by the pipeline.  Unlike a multiplexed client dispatcher, a
 * multiplexed server dispatcher needs no state, and the sequence id's
 * can just be copied from the request to the response in the pipeline.
 */
 // MultiplexServerDispatcher在read到一个请求之后立刻发起异步调用，每一次异步调用结束之后直接发送响应，
 // 也就是响应的发送顺序和请求没有直接对应关系
 // 为了client端知道返回的是哪个request的response,可以将id隐藏在请求和响应数据结构中
template <typename Req, typename Resp = Req>
class MultiplexServerDispatcher : public HandlerAdapter<Req, Resp> {
 public:

  typedef typename HandlerAdapter<Req, Resp>::Context Context;

  explicit MultiplexServerDispatcher(Service<Req, Resp>* service)
      : service_(service) {}

  void read(Context* ctx, Req in) override {
    (*service_)(std::move(in)).thenValue([ctx](Resp resp) {
      ctx->fireWrite(std::move(resp));
    });
  }

 private:
  Service<Req, Resp>* service_;
};

} // namespace wangle
