/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include <gflags/gflags.h>

#include <folly/wangle/bootstrap/ServerBootstrap.h>
#include <folly/wangle/channel/AsyncSocketHandler.h>
#include <folly/wangle/channel/FileRegion.h>
#include <folly/wangle/codec/LineBasedFrameDecoder.h>
#include <folly/wangle/codec/StringCodec.h>
#include <sys/sendfile.h>

using namespace folly;
using namespace folly::wangle;

DEFINE_int32(port, 11219, "test file server port");

typedef Pipeline<IOBufQueue&, std::string> FileServerPipeline;

class FileServerHandler : public HandlerAdapter<std::string> {
 public:
  void read(Context* ctx, std::string filename) override {
    if (filename == "bye") {
      close(ctx);
    }

    int fd = open(filename.c_str(), O_RDONLY);
    if (fd == -1) {
      write(ctx, sformat("Error opening {}: {}\r\n",
                         filename,
                         strerror(errno)));
      return;
    }

    struct stat buf;
    if (fstat(fd, &buf) == -1) {
      write(ctx, sformat("Could not stat file {}: {}\r\n",
                         filename,
                         strerror(errno)));
      return;
    }

    FileRegion fileRegion(fd, 0, buf.st_size);
    FileServerPipeline::DestructorGuard dg(ctx->getPipeline());
    fileRegion.transferTo(ctx->getTransport())
      .onError([this, dg, ctx, filename](const std::exception& e){
        write(ctx, sformat("Error sending file {}: {}\r\n",
                           filename,
                           exceptionStr(e)));
      });
  }

  void readException(Context* ctx, exception_wrapper ew) override {
    write(ctx, sformat("Error: {}\r\n", exceptionStr(ew))).then([this, ctx]{
      close(ctx);
    });
  }

  void transportActive(Context* ctx) override {
    SocketAddress localAddress;
    ctx->getTransport()->getLocalAddress(&localAddress);
    write(ctx, "Welcome to " + localAddress.describe() + "!\r\n");
    write(ctx, "Type the name of a file and it will be streamed to you!\r\n");
    write(ctx, "Type 'bye' to exit.\r\n");
  }
};

class FileServerPipelineFactory : public PipelineFactory<FileServerPipeline> {
 public:
  std::unique_ptr<FileServerPipeline, folly::DelayedDestruction::Destructor>
  newPipeline(std::shared_ptr<AsyncSocket> sock) {

    std::unique_ptr<FileServerPipeline, folly::DelayedDestruction::Destructor>
      pipeline(new FileServerPipeline);
    pipeline->addBack(AsyncSocketHandler(sock));
    pipeline->addBack(LineBasedFrameDecoder());
    pipeline->addBack(StringCodec());
    pipeline->addBack(FileServerHandler());
    pipeline->finalize();

    return std::move(pipeline);
  }
};

int main(int argc, char** argv) {
  google::ParseCommandLineFlags(&argc, &argv, true);

  ServerBootstrap<FileServerPipeline> server;
  server.childPipeline(std::make_shared<FileServerPipelineFactory>());
  server.bind(FLAGS_port);
  server.waitForStop();

  return 0;
}
