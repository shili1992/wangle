![Aaron Swartz]（https://raw.githubusercontent.com/shili1992/picture/master/frame%20stack.png]

![](C:\Users\shili\Desktop\Image [2].png)



一个节点运行过程为：

 上一个节点 调用 *HandlerContext::fire_read，  fire_read函数中 调用 下一个节点的 （InboundLink 也是下一个context）read函数。context的read 函数 调用 context中handler 的read函数，   handler的read函数调用这个节点context的 fire_read函数， 执行下一个节点的操作。* 这样整个调用就串起来了。


 ContextImpl就是最终的Context实现，也就是要被添加到Pipeline中（比如使用addBack）的容器（ctxs_，inCtxs_，outCtxs_）的最终Context，在最后的finalize方法中还会进一步将容器中的Context组装成front_和back_单向链表。