client发送一个write请求，

线程1的调用栈（发送请求这个线程）
![Aaron Swartz](https://raw.githubusercontent.com/shili1992/picture/master/echoClient%20send%20write%20request_1.png)

pipline从back_开始传播事件， EchoHandler -> StringCodec -> LineBasedFrameDecoder -> EventBaseHandler.

在传播到 EventBaseHandler的时候， 切换了到绑定的IO线程中，  会保证最终在IO线程中完成真正的网络IO
![Aaron Swartz](https://raw.githubusercontent.com/shili1992/picture/master/echoClient%20send%20write%20request2.png)

线程2， 从线程1切换到线程2(IO线程)， 并且发送真正的request.
![Aaron Swartz](https://raw.githubusercontent.com/shili1992/picture/master/echoClient%20send%20write%20request3.png)

线程关系如下图所示
![Aaron Swartz](https://raw.githubusercontent.com/shili1992/picture/master/echoClient%20send%20write%20request4.png)



