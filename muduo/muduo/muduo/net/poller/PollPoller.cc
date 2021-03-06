// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)

#include <muduo/net/poller/PollPoller.h>

#include <muduo/base/Logging.h>
#include <muduo/base/Types.h>
#include <muduo/net/Channel.h>

#include <assert.h>
#include <errno.h>
#include <poll.h>

using namespace muduo;
using namespace muduo::net;

PollPoller::PollPoller(EventLoop* loop)
  : Poller(loop)
{
}

PollPoller::~PollPoller()
{
}

Timestamp PollPoller::poll(int timeoutMs, ChannelList* activeChannels)
{
  // XXX pollfds_ shouldn't change    //传入数组首地址，大小，超时
  int numEvents = ::poll(&*pollfds_.begin(), pollfds_.size(), timeoutMs);
  int savedErrno = errno;
  Timestamp now(Timestamp::now());  //时间戳
  if (numEvents > 0)  //说明有事件发生
  {
    LOG_TRACE << numEvents << " events happended";
    fillActiveChannels(numEvents, activeChannels);  //放入活跃事件通道中
  }
  else if (numEvents == 0)
  {
    LOG_TRACE << " nothing happended";
  }
  else
  {
    if (savedErrno != EINTR)
    {
      errno = savedErrno;
      LOG_SYSERR << "PollPoller::poll()";
    }
  }
  return now;
}

//向活跃事件通道数组放入活跃事件
void PollPoller::fillActiveChannels(int numEvents,
                                    ChannelList* activeChannels) const
{
  for (PollFdList::const_iterator pfd = pollfds_.begin();
      pfd != pollfds_.end() && numEvents > 0; ++pfd)  
  {
    if (pfd->revents > 0)  //>=说明产生了事件
    {
      --numEvents;  //处理一个减减
      //map<int, channel*>  文件描述符和通道指针的map
      ChannelMap::const_iterator ch = channels_.find(pfd->fd);//调用map.find函数
      assert(ch != channels_.end()); //断言找到事件
      Channel* channel = ch->second;   //获取事件
      assert(channel->fd() == pfd->fd);  //断言相等
      channel->set_revents(pfd->revents);   //设置要返回的事件类型
      // pfd->revents = 0;
      activeChannels->push_back(channel);  //加入活跃事件数组
    }
  }
}

//用于注册或者更新通道
void PollPoller::updateChannel(Channel* channel)
{
  Poller::assertInLoopThread();  //断言只能在I/O线程中调用
  LOG_TRACE << "fd = " << channel->fd() << " events = " << channel->events();
  if (channel->index() < 0)  //如果小于0，说明还没有注册�
  {
    // a new one, add to pollfds_
    assert(channels_.find(channel->fd()) == channels_.end());//断言查找不到
    struct pollfd pfd;
    pfd.fd = channel->fd();
    pfd.events = static_cast<short>(channel->events());
    pfd.revents = 0;  
    pollfds_.push_back(pfd);  //合成pollfd
    int idx = static_cast<int>(pollfds_.size())-1;  //idx放在末尾，因为是最新的，这个idx是pollfd_的下标
    channel->set_index(idx);  //idx是channel的成员变量
    channels_[pfd.fd] = channel; //加入map中
  }
  else   //如果大于0，说明是更新一个已存在的通道
  {
    // update existing one
    assert(channels_.find(channel->fd()) != channels_.end());
    assert(channels_[channel->fd()] == channel);
    int idx = channel->index();   //取出channel的下标
    assert(0 <= idx && idx < static_cast<int>(pollfds_.size()));
    struct pollfd& pfd = pollfds_[idx];  //获取pollfd，以引用方式提高效率
    assert(pfd.fd == channel->fd() || pfd.fd == -channel->fd()-1);  //-channel->fd()-1是因为匹配下面暂时不关注状态的设置
    pfd.events = static_cast<short>(channel->events());   //更新events
    pfd.revents = 0;
    //将一个通道暂时更改为不关注事件，但不从Poller中移除通道，可能下一次还会用
    if (channel->isNoneEvent())  
    {
      // ignore this pollfd
      //暂时忽略该文件描述符的时间
      //这里pfd.fd可以直接设置为-1
      pfd.fd = -channel->fd()-1;   //这样子设置是为了removeChannel优化
    }
  }
}

//这个是真正移除pollfd的程序
void PollPoller::removeChannel(Channel* channel)
{
  Poller::assertInLoopThread();
  LOG_TRACE << "fd = " << channel->fd();
  assert(channels_.find(channel->fd()) != channels_.end());//删除必须能找到
  assert(channels_[channel->fd()] == channel);  //一定对应
  assert(channel->isNoneEvent());  //一定已经没有事件上的关注了，所以在调用removeChannel之前必须
  int idx = channel->index();  //取出在pollfds_数组下标中的位置
  assert(0 <= idx && idx < static_cast<int>(pollfds_.size()));
  const struct pollfd& pfd = pollfds_[idx]; (void)pfd;  //得到pollfd
  assert(pfd.fd == -channel->fd()-1 && pfd.events == channel->events());
  size_t n = channels_.erase(channel->fd()); //使用键值移除从map中移除
  assert(n == 1); (void)n;
  if (implicit_cast<size_t>(idx) == pollfds_.size()-1)  //如果是最后一个
  {
    pollfds_.pop_back();  //直接pop_back()
  }
  else  //不是vector最后一个，这个问题就来了！vector移除前面元素可能会涉及到吧后面元素挪动，所以不可取。
  {       //这里采用的是交换的方法，类似于图算法中删除顶点，我们将要移除的元素和最后一个元素交换即可。
  			//这里算法时间复杂度是O(1)
    int channelAtEnd = pollfds_.back().fd;   //得到最后一个元素
    iter_swap(pollfds_.begin()+idx, pollfds_.end()-1);  //交换
    if (channelAtEnd < 0)  //如果最后一个<0
    {
      channelAtEnd = -channelAtEnd-1;  //把它还原出来，得到真实的fd，这就是不使用-1的原因，我们不关注它了
      															//把它改为-fd-1，然后删除的时候可以再还原回来。如果改为-1就不可以了。
    }
    channels_[channelAtEnd]->set_index(idx); //对该真实的fd更新下标
    pollfds_.pop_back();  //弹出末尾元素
  }
}

