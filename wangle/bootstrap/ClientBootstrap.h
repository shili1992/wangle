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

#include <folly/io/async/AsyncSSLSocket.h>
#include <folly/io/async/AsyncSocket.h>
#include <folly/io/async/DestructorCheck.h>
#include <folly/io/async/EventBaseManager.h>
#include <wangle/bootstrap/BaseClientBootstrap.h>
#include <wangle/channel/Pipeline.h>
#include <folly/executors/IOThreadPoolExecutor.h>

using folly::AsyncSSLSocket;

namespace wangle {

/*
 * A thin wrapper around Pipeline and AsyncSocket to match
 * ServerBootstrap.  On connect() a new pipeline is created.
 */
template <typename Pipeline>
class ClientBootstrap : public BaseClientBootstrap<Pipeline>,
                        public folly::DestructorCheck {
  class ConnectCallback : public folly::AsyncSocket::ConnectCallback {
   public:
    ConnectCallback(
        folly::Promise<Pipeline*> promise,
        ClientBootstrap* bootstrap,
        std::shared_ptr<folly::AsyncSocket> socket,
        SSLSessionEstablishedCallbackUniquePtr sslSessionEstablishedCallback)
        : promise_(std::move(promise)),
          bootstrap_(bootstrap),
          socket_(socket),
          safety_(*bootstrap),
          sslSessionEstablishedCallback_(
              std::move(sslSessionEstablishedCallback)) {}

    void connectSuccess() noexcept override {
      if (!safety_.destroyed()) {

        if (sslSessionEstablishedCallback_) {
          AsyncSSLSocket* sslSocket =
            dynamic_cast<AsyncSSLSocket*>(socket_.get());
          if (sslSocket && !sslSocket->getSSLSessionReused()) {
            sslSessionEstablishedCallback_->onEstablished(
              sslSocket->getSSLSession());
          }
        }
        bootstrap_->makePipeline(std::move(socket_));  // 会使用pipelineFactory_的newPipeline进行创建
        if (bootstrap_->getPipeline()) {
          bootstrap_->getPipeline()->transportActive();  // 会在这个Pipeline中传递transportActive事件
        }
        promise_.setValue(bootstrap_->getPipeline());  // 将这个pipeline返回
      }
      delete this;
    }

    void connectErr(const folly::AsyncSocketException& ex) noexcept override {
      promise_.setException(
        folly::make_exception_wrapper<folly::AsyncSocketException>(ex));
      delete this;
    }
   private:
    folly::Promise<Pipeline*> promise_;
    ClientBootstrap* bootstrap_;
    std::shared_ptr<folly::AsyncSocket> socket_;  //用于初始化AsyncSocketHandler用于网络收发
    folly::DestructorCheck::Safety safety_;
    SSLSessionEstablishedCallbackUniquePtr sslSessionEstablishedCallback_;
  };

 public:
  ClientBootstrap() {
  }

  // 设置IO线程池
  ClientBootstrap* group(
      std::shared_ptr<folly::IOThreadPoolExecutor> group) {
    group_ = group;
    return this;
  }

  ClientBootstrap* bind(int port) {
    port_ = port;
    return this;
  }

  folly::Future<Pipeline*> connect(
      const folly::SocketAddress& address,
      std::chrono::milliseconds timeout =
          std::chrono::milliseconds(0)) override {
      // 如果指定了io线程池，就从IO线程池中取eventbase，否则就从folly::EventBaseManager取
    auto base = (group_)
      ? group_->getEventBase()
      : folly::EventBaseManager::get()->getEventBase();
    folly::Future<Pipeline*> retval((Pipeline*)nullptr);
    base->runImmediatelyOrRunInEventBaseThreadAndWait([&](){
      std::shared_ptr<folly::AsyncSocket> socket;
      if (this->sslContext_) {
        auto sslSocket = folly::AsyncSSLSocket::newSocket(
            this->sslContext_,
            base,
            this->deferSecurityNegotiation_);
        if (!this->sni_.empty()) {
          sslSocket->setServerName(this->sni_);
        }
        if (this->sslSession_) {
          sslSocket->setSSLSession(this->sslSession_, true);
        }
        socket = sslSocket;
      } else {
          // 创建一个异步socket
        socket = folly::AsyncSocket::newSocket(base);
      }
      folly::Promise<Pipeline*> promise;
      retval = promise.getFuture();
        // 发起异步连接，并将promise传给异步回调
      socket->connect(
          new ConnectCallback(
              std::move(promise),
              this,
              socket,  // connect失败成功后的回调
              std::move(this->sslSessionEstablishedCallback_)),
          address,
          timeout.count());
    });
    return retval;
  }

  ~ClientBootstrap() override = default;

 protected:
  int port_;
  std::shared_ptr<folly::IOThreadPoolExecutor> group_; //client 使用的 io线程池
};

class ClientBootstrapFactory
    : public BaseClientBootstrapFactory<BaseClientBootstrap<>> {
 public:
  ClientBootstrapFactory() {}

  BaseClientBootstrap<>::Ptr newClient() override {
    return std::make_unique<ClientBootstrap<DefaultPipeline>>();
  }
};

} // namespace wangle
