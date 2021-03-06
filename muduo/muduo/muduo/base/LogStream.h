#ifndef MUDUO_BASE_LOGSTREAM_H
#define MUDUO_BASE_LOGSTREAM_H

#include <muduo/base/StringPiece.h>
#include <muduo/base/Types.h>
#include <assert.h>
#include <string.h> // memcpy
#ifndef MUDUO_STD_STRING
#include <string>
#endif
#include <boost/noncopyable.hpp>

namespace muduo
{

namespace detail
{

//缓冲区大小的配置
const int kSmallBuffer = 4000;
const int kLargeBuffer = 4000*1000;

template<int SIZE>
class FixedBuffer : boost::noncopyable
{
 public:
  FixedBuffer()
    : cur_(data_)
  {
    setCookie(cookieStart);  //设置cookie，muduo库这个函数目前还没加入功能，所以可以不用管
  }

  ~FixedBuffer()
  {
    setCookie(cookieEnd);
  }

  void append(const char* /*restrict*/ buf, size_t len)  //添加数据
  {
    // FIXME: append partially
    if (implicit_cast<size_t>(avail()) > len)  //如果可用数据足够，就拷贝过去，同时移动当前指针。
    {
      memcpy(cur_, buf, len);
      cur_ += len;
    }
  }

  const char* data() const { return data_; }  //返回首地址
  int length() const { return static_cast<int>(cur_ - data_); }   //返回缓冲区已有数据长度

  // write to data_ directly
  char* current() { return cur_; }  //返回当前数据末端地址
  int avail() const { return static_cast<int>(end() - cur_); }  //返回剩余可用地址
  void add(size_t len) { cur_ += len; }  //cur前移

  void reset() { cur_ = data_; }   //重置，不清数据，只需要让cur指回首地址即可
  void bzero() { ::bzero(data_, sizeof data_); }  //清零
  
  // for used by GDB
  const char* debugString();
  void setCookie(void (*cookie)()) { cookie_ = cookie; }
  // for used by unit test
  string toString() const { return string(data_, length()); }
  StringPiece toStringPiece() const { return StringPiece(data_, length()); }  //返回string类型

 private:
  const char* end() const { return data_ + sizeof data_; } //返回尾指针
  // Must be outline function for cookies.
  static void cookieStart();
  static void cookieEnd();

  void (*cookie_)();
  char data_[SIZE];   //缓冲区数组
  char* cur_;      //cur永远指向已有数据的最右端，data->cur->end结构�
};

}

class LogStream : boost::noncopyable
{
  typedef LogStream self;
 public:
  typedef detail::FixedBuffer<detail::kSmallBuffer> Buffer;  //缓冲区，使用smallbuffer

  //针对不同类型重载了operator<<
  self& operator<<(bool v)   //别的类如果调用LogStream的<<实际上是把内容追加到LogStream的缓冲区。
  {
    buffer_.append(v ? "1" : "0", 1);  //追加
    return *this;
  }
  self& operator<<(short);
  self& operator<<(unsigned short);
  self& operator<<(int);
  self& operator<<(unsigned int);
  self& operator<<(long);
  self& operator<<(unsigned long);
  self& operator<<(long long);
  self& operator<<(unsigned long long);

  self& operator<<(const void*);

  self& operator<<(float v)
  {
    *this << static_cast<double>(v);    //把float类型转化为double类型，调用下面的重载函数
    return *this;
  }
  self& operator<<(double);
  // self& operator<<(long double);

  self& operator<<(char v)
  {
    buffer_.append(&v, 1);
    return *this;
  }

  // self& operator<<(signed char);
  // self& operator<<(unsigned char);

  self& operator<<(const char* str)
  {
    if (str)
    {
      buffer_.append(str, strlen(str));
    }
    else
    {
      buffer_.append("(null)", 6);
    }
    return *this;
  }

  self& operator<<(const unsigned char* str)
  {
    return operator<<(reinterpret_cast<const char*>(str));
  }

  self& operator<<(const string& v)
  {
    buffer_.append(v.c_str(), v.size());
    return *this;
  }

#ifndef MUDUO_STD_STRING
  self& operator<<(const std::string& v)
  {
    buffer_.append(v.c_str(), v.size());
    return *this;
  }
#endif

  self& operator<<(const StringPiece& v)
  {
    buffer_.append(v.data(), v.size());
    return *this;
  }

  self& operator<<(const Buffer& v)
  {
    *this << v.toStringPiece();
    return *this;
  }

  void append(const char* data, int len) { buffer_.append(data, len); }
  const Buffer& buffer() const { return buffer_; }
  void resetBuffer() { buffer_.reset(); }

 private:
  void staticCheck();

  template<typename T>
  void formatInteger(T);

  Buffer buffer_;

  static const int kMaxNumericSize = 32;
};

class Fmt // : boost::noncopyable
{
 public:
  template<typename T>
  Fmt(const char* fmt, T val);   //把整数按照T类型格式化到buffer中

  const char* data() const { return buf_; }
  int length() const { return length_; }  
 private:
  char buf_[32];
  int length_;
};

inline LogStream& operator<<(LogStream& s, const Fmt& fmt)
{
  s.append(fmt.data(), fmt.length());
  return s;
}

}
#endif  // MUDUO_BASE_LOGSTREAM_H

