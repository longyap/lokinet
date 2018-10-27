#include <llarp/ev.h>
#include <llarp/logic.h>
#include "mem.hpp"

#define EV_TICK_INTERVAL 100

// apparently current Solaris will emulate epoll.
#if __linux__ || __sun__
#include "ev_epoll.hpp"
#endif
#if defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) \
    || (__APPLE__ && __MACH__)
#include "ev_kqueue.hpp"
#endif
#if defined(_WIN32) || defined(_WIN64) || defined(__NT__)
#include "ev_win32.hpp"
#endif

void
llarp_ev_loop_alloc(struct llarp_ev_loop **ev)
{
#if __linux__ || __sun__
  *ev = new llarp_epoll_loop;
#endif
#if defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) \
    || (__APPLE__ && __MACH__)
  *ev = new llarp_kqueue_loop;
#endif
#if defined(_WIN32) || defined(_WIN64) || defined(__NT__)
  *ev = new llarp_win32_loop;
#endif
  (*ev)->init();
  (*ev)->_now = llarp_time_now_ms();
}

void
llarp_ev_loop_free(struct llarp_ev_loop **ev)
{
  delete *ev;
  *ev = nullptr;
}

int
llarp_ev_loop_run(struct llarp_ev_loop *ev, struct llarp_logic *logic)
{
  while(ev->running())
  {
    ev->_now = llarp_time_now_ms();
    ev->tick(EV_TICK_INTERVAL);
    if(ev->running())
      llarp_logic_tick(logic, ev->_now);
  }
  return 0;
}

void
llarp_ev_loop_run_single_process(struct llarp_ev_loop *ev,
                                 struct llarp_threadpool *tp,
                                 struct llarp_logic *logic)
{
  while(ev->running())
  {
    ev->_now = llarp_time_now_ms();
    ev->tick(EV_TICK_INTERVAL);
    if(ev->running())
    {
      llarp_logic_tick_async(logic, ev->_now);
      llarp_threadpool_tick(tp);
    }
  }
}

int
llarp_ev_add_udp(struct llarp_ev_loop *ev, struct llarp_udp_io *udp,
                 const struct sockaddr *src)
{
  udp->parent = ev;
  if(ev->udp_listen(udp, src))
    return 0;
  return -1;
}

int
llarp_ev_close_udp(struct llarp_udp_io *udp)
{
  if(udp->parent->udp_close(udp))
    return 0;
  return -1;
}

llarp_time_t
llarp_ev_loop_time_now_ms(struct llarp_ev_loop *loop)
{
  return loop->_now;
}

void
llarp_ev_loop_stop(struct llarp_ev_loop *loop)
{
  loop->stop();
}

int
llarp_ev_udp_sendto(struct llarp_udp_io *udp, const sockaddr *to,
                    const void *buf, size_t sz)
{
  auto ret = static_cast< llarp::ev_io * >(udp->impl)->sendto(to, buf, sz);
  if(ret == -1 && errno != 0)
  {
    llarp::LogWarn("sendto failed ", strerror(errno));
    errno = 0;
  }
  return ret;
}

bool
llarp_ev_add_tun(struct llarp_ev_loop *loop, struct llarp_tun_io *tun)
{
  auto dev  = loop->create_tun(tun);
  tun->impl = dev;
  if(dev)
  {
    return loop->add_ev(dev);
  }
  return false;
}

bool
llarp_tcp_conn_async_write(struct llarp_tcp_conn *conn, const void *pkt,
                           size_t sz)
{
  const byte_t *ptr     = (const byte_t *)pkt;
  llarp::tcp_conn *impl = static_cast< llarp::tcp_conn * >(conn->impl);
  if(impl->_shouldClose)
    return false;
  while(sz > EV_WRITE_BUF_SZ)
  {
    if(!impl->queue_write((const byte_t *)ptr, EV_WRITE_BUF_SZ))
      return false;
    ptr += EV_WRITE_BUF_SZ;
    sz -= EV_WRITE_BUF_SZ;
  }
  return impl->queue_write(ptr, sz);
}

bool
llarp_tcp_serve(struct llarp_ev_loop *loop, struct llarp_tcp_acceptor *tcp,
                const struct sockaddr *bindaddr)
{
  tcp->loop          = loop;
  llarp::ev_io *impl = loop->bind_tcp(tcp, bindaddr);
  if(impl)
  {
    tcp->impl = impl;
    return loop->add_ev(impl);
  }
  return false;
}

void
llarp_tcp_acceptor_close(struct llarp_tcp_acceptor *tcp)
{
  llarp::ev_io *impl = static_cast< llarp::ev_io * >(tcp->user);
  tcp->impl          = nullptr;
  tcp->loop->close_ev(impl);
  if(tcp->closed)
    tcp->closed(tcp);
  // dont free acceptor because it may be stack allocated
}

bool
llarp_ev_tun_async_write(struct llarp_tun_io *tun, const void *buf, size_t sz)
{
  if(sz > EV_WRITE_BUF_SZ)
  {
    llarp::LogWarn("packet too big, ", sz, " > ", EV_WRITE_BUF_SZ);
    return false;
  }
  return static_cast< llarp::tun * >(tun->impl)->queue_write(
      (const byte_t *)buf, sz);
}

void
llarp_tcp_conn_close(struct llarp_tcp_conn *conn)
{
  static_cast< llarp::tcp_conn * >(conn->impl)->_shouldClose = true;
}

namespace llarp
{
  bool
  tcp_conn::tick()
  {
    if(_shouldClose)
    {
      if(tcp && tcp->closed)
        tcp->closed(tcp);
      return false;
    }
    else if(tcp->tick)
      tcp->tick(tcp);
    return true;
  }

  int
  tcp_serv::read(void *, size_t)
  {
#ifndef _WIN32
    int new_fd = ::accept(fd, nullptr, nullptr);
    if(new_fd == -1)
    {
      llarp::LogError("failed to accept on ", fd, ":", strerror(errno));
      return -1;
    }
#else
    SOCKET new_fd = ::accept(std::get< SOCKET >(fd), nullptr, nullptr);
    if(new_fd == INVALID_SOCKET)
    {
      llarp::LogError("failed to accept on ", std::get< SOCKET >(fd), ":",
                      strerror(errno));
      return -1;
    }
#endif
    llarp_tcp_conn *conn = new llarp_tcp_conn;
    // zero out callbacks
    conn->tick   = nullptr;
    conn->closed = nullptr;
    conn->read   = nullptr;
    // build handler
    llarp::tcp_conn *connimpl = new tcp_conn(new_fd, conn);
    conn->impl                = connimpl;
    conn->loop                = loop;
    if(loop->add_ev(connimpl, true))
    {
      // call callback
      if(tcp->accepted)
        tcp->accepted(tcp, conn);
      return 0;
    }
    // cleanup error
    delete conn;
    delete connimpl;
    return -1;
  }

}  // namespace llarp

// they're effectively alike, save for the fact that we must not
// get file descriptors below zero
#ifndef _WIN32
llarp::ev_io *
llarp_ev_loop::bind_tcp(llarp_tcp_acceptor *tcp, const sockaddr *bindaddr)
{
  int fd = ::socket(bindaddr->sa_family, SOCK_STREAM, 0);
  if(fd == -1)
    return nullptr;
  socklen_t sz = sizeof(sockaddr_in);
  if(bindaddr->sa_family == AF_INET6)
  {
    sz = sizeof(sockaddr_in6);
  }
  else if(bindaddr->sa_family == AF_UNIX)
  {
    sz = sizeof(sockaddr_un);
  }
  if(::bind(fd, bindaddr, sz) == -1)
  {
    ::close(fd);
    return nullptr;
  }
  if(::listen(fd, 5) == -1)
  {
    ::close(fd);
    return nullptr;
  }
  llarp::ev_io *serv = new llarp::tcp_serv(this, fd, tcp);
  tcp->impl          = serv;
  return serv;
}
#else
llarp::ev_io *
llarp_ev_loop::bind_tcp(llarp_tcp_acceptor *tcp, const sockaddr *bindaddr)
{
  DWORD on = 1;
  SOCKET fd = ::socket(bindaddr->sa_family, SOCK_STREAM, 0);
  if(fd == INVALID_SOCKET)
    return nullptr;
  socklen_t sz = sizeof(sockaddr_in);
  if(bindaddr->sa_family == AF_INET6)
  {
    sz = sizeof(sockaddr_in6);
  }
  // keep. inexplicably, windows now has unix domain sockets
  // for now, use the ID numbers directly until this comes out of
  // beta
  else if(bindaddr->sa_family == AF_UNIX)
  {
    sz = 110;  // current size in 10.0.17763, verify each time the beta PSDK
               // is updated
  }
  if(::bind(fd, bindaddr, sz) == SOCKET_ERROR)
  {
    ::closesocket(fd);
    return nullptr;
  }
  if(::listen(fd, 5) == SOCKET_ERROR)
  {
    ::closesocket(fd);
    return nullptr;
  }
  llarp::ev_io *serv = new llarp::tcp_serv(this, fd, tcp);
  tcp->impl = serv;
  // We're non-blocking now, but can't really make use of it
  // until we cut over to WSA* functions
  ioctlsocket(fd, FIONBIO, &on);
  return serv;
}
#endif
