// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)
//

#include <muduo/net/TcpClient.h>

#include <muduo/base/Logging.h>
#include <muduo/net/Connector.h>
#include <muduo/net/EventLoop.h>
#include <muduo/net/SocketsOps.h>

#include <boost/bind.hpp>

#include <stdio.h>  // snprintf

using namespace muduo;
using namespace muduo::net;

// TcpClient::TcpClient(EventLoop* loop)
//   : loop_(loop)
// {
// }

// TcpClient::TcpClient(EventLoop* loop, const string& host, uint16_t port)
//   : loop_(CHECK_NOTNULL(loop)),
//     serverAddr_(host, port)
// {
// }

namespace muduo
{
namespace net
{
namespace detail
{

//不重连的销毁方式
void removeConnection(EventLoop* loop, const TcpConnectionPtr& conn)
{
  loop->queueInLoop(boost::bind(&TcpConnection::connectDestroyed, conn));
}

//移除
void removeConnector(const ConnectorPtr& connector)
{
  //connector->
}

}
}
}

TcpClient::TcpClient(EventLoop* loop,
                     const InetAddress& serverAddr,
                     const string& nameArg)
  : loop_(CHECK_NOTNULL(loop)),
    connector_(new Connector(loop, serverAddr)),
    name_(nameArg),
    connectionCallback_(defaultConnectionCallback),
    messageCallback_(defaultMessageCallback),
    retry_(false),
    connect_(true),
    nextConnId_(1)
{
  //一旦连接建立连接，回调newConnection
  connector_->setNewConnectionCallback(
      boost::bind(&TcpClient::newConnection, this, _1));
  // FIXME setConnectFailedCallback
  LOG_INFO << "TcpClient::TcpClient[" << name_
           << "] - connector " << get_pointer(connector_);
}

TcpClient::~TcpClient()
{
  LOG_INFO << "TcpClient::~TcpClient[" << name_
           << "] - connector " << get_pointer(connector_);
  TcpConnectionPtr conn;
  bool unique = false;
  {
    MutexLockGuard lock(mutex_);
    unique = connection_.unique();  //shared_ptr是否被唯一持有
    conn = connection_;
  }
  if (conn)  //如果已经建立的连接
  {
    assert(loop_ == conn->getLoop());
    // FIXME: not 100% safe, if we are in different thread
    //从新设置TcpConnection中的closeCallback_为detail::removeConnection
    //不能调用TcpClient的removeConnection，因为它具有自动重连的功能
    CloseCallback cb = boost::bind(&detail::removeConnection, loop_, _1);
    loop_->runInLoop(
        boost::bind(&TcpConnection::setCloseCallback, conn, cb));
    if (unique) //引用计数为1
    {
      conn->forceClose();  //直接关闭
    }
  }
  else
  {
    //这种情况，说明Connector处于未连接状态，将connector_停止
    connector_->stop();
    // FIXME: HACK
    loop_->runAfter(1, boost::bind(&detail::removeConnector, connector_));
  }
}

void TcpClient::connect()
{
  // FIXME: check state
  LOG_INFO << "TcpClient::connect[" << name_ << "] - connecting to "
           << connector_->serverAddress().toIpPort();
  connect_ = true;
  connector_->start();
}

//用于连接已建立的情况下，关闭连接
void TcpClient::disconnect()
{
  connect_ = false;

  {
    MutexLockGuard lock(mutex_);
    if (connection_)
    {
      connection_->shutdown();
    }
  }
}

//可能连接尚未建立，停止connector_
void TcpClient::stop()
{
  connect_ = false;
  connector_->stop();
}

void TcpClient::newConnection(int sockfd)
{
  loop_->assertInLoopThread();
  InetAddress peerAddr(sockets::getPeerAddr(sockfd));
  char buf[32];
  snprintf(buf, sizeof buf, ":%s#%d", peerAddr.toIpPort().c_str(), nextConnId_);
  ++nextConnId_;
  string connName = name_ + buf;

  InetAddress localAddr(sockets::getLocalAddr(sockfd));
  // FIXME poll with zero timeout to double confirm the new connection
  // FIXME use make_shared if necessary
  //创建一个TcpConnection对象，智能指针。
  //根据Connector中的handleWrite()函数，连接建立后会把sockfd从poller中移除，以后不会再关注可写事件了
  //否则会出现busy loop，因为已连接套接字一直处于可写状态
  TcpConnectionPtr conn(new TcpConnection(loop_,
                                          connName,
                                          sockfd,
                                          localAddr,
                                          peerAddr));
  
  conn->setConnectionCallback(connectionCallback_);
  conn->setMessageCallback(messageCallback_);
  conn->setWriteCompleteCallback(writeCompleteCallback_);
  conn->setCloseCallback(
      boost::bind(&TcpClient::removeConnection, this, _1)); // FIXME: unsafe
  {
    MutexLockGuard lock(mutex_);
    connection_ = conn;   //保存TcpConnection
  }
  conn->connectEstablished();  //这里会关注可读事件，并且回调connectionCallback_，
}

//连接断开
void TcpClient::removeConnection(const TcpConnectionPtr& conn)
{
  loop_->assertInLoopThread();
  assert(loop_ == conn->getLoop());

  {
    MutexLockGuard lock(mutex_);
    assert(connection_ == conn);
    connection_.reset();   //重置
  }

  //I/O线程中销毁
  loop_->queueInLoop(boost::bind(&TcpConnection::connectDestroyed, conn));
  if (retry_ && connect_)  //是否发起重连
  {
    LOG_INFO << "TcpClient::connect[" << name_ << "] - Reconnecting to "
             << connector_->serverAddress().toIpPort();
    //这里的重连是连接成功后断开的重连，所以实际上是重启
    connector_->restart();
  }
}

