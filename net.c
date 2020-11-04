#include "dat.h"
#include <netdb.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#ifdef HAVE_LIBSYSTEMD
#include <systemd/sd-daemon.h>
#endif

static int
set_nonblocking(int fd)
{
    int flags, r;

    flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        twarn("getting flags");
        return -1;
    }
    r = fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    if (r == -1) {
        twarn("setting O_NONBLOCK");
        return -1;
    }
    return 0;
}

static int
make_inet_socket(char *host, char *port)
{
    int fd = -1, flags, r;
    struct linger linger = {0, 0};
    struct addrinfo *airoot, *ai, hints;

    memset(&hints, 0, sizeof(hints));

    hints.ai_family = AF_UNSPEC; // 返回协议簇类型: AF_INET(IPv4)、AF_INET6(IPv6)、AF_UNSPEC(IPv4 and IPv6)
    hints.ai_socktype = SOCK_STREAM; // 返回地址的socket类型: 常见 SOCK_STREAM()、SOCK_DGRAM、SOCK_RAW, 设置为0表示所有类型都可以。
    hints.ai_flags = AI_PASSIVE; // 附加选项参数 详见: https://www.cnblogs.com/fnlingnzb-learner/p/7542770.html

    // 协议地址转换 host ip转换成socket地址
    // @host 域名或ip
    // @port 端口 或者常用服务名称如"ftp"、"http"等
    // @hints 设置参数
    // @airoot 返回结果
    r = getaddrinfo(host, port, &hints, &airoot);
    if (r != 0) {
        twarnx("getaddrinfo(): %s", gai_strerror(r));
        return -1;
    }

    // 返回是一个地址链表
    for (ai = airoot; ai; ai = ai->ai_next) {
        // 通过协议簇类型、socket类型(SOCK_STREAM: 流式协议、SOCK_DGRAM: 数据报协议)、具体协议(TCP|UDP)、创建一个文件描述符
        // 为什么需要ai_family和ai_protocol 两个参数，因为流式协议不止有TCP 数据报协议也不止有UDP
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd == -1) {
            twarn("socket()");
            continue;
        }

        // 设置非阻塞模式
        r = set_nonblocking(fd);
        if (r == -1) {
            close(fd);
            continue;
        }

        // socket 参数设置
        flags = 1;
        // @fd 前面创建的描述符
        // @level 被设置选项的级别
        // @option_name 具体设置选项
        // @optval 具体设置选项值的缓冲区
        // @optlen 缓冲区长度

        // SO_REUSEADDR 允许地址重用 使用已经正在使用中的地址
        r = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &flags, sizeof flags);
        if (r == -1) {
            twarn("setting SO_REUSEADDR on fd %d", fd);
            close(fd);
            continue;
        }

        // SO_KEEPALIVE 是否发送心跳包保活
        r = setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &flags, sizeof flags);
        if (r == -1) {
            twarn("setting SO_KEEPALIVE on fd %d", fd);
            close(fd);
            continue;
        }
        // SO_LINGER close时如果有未发送的数据则等待数据发送完成
        r = setsockopt(fd, SOL_SOCKET, SO_LINGER, &linger, sizeof linger);
        if (r == -1) {
            twarn("setting SO_LINGER on fd %d", fd);
            close(fd);
            continue;
        }
        // TCP_NODELAY 禁止使用 Nagle 算法
        // 参考: https://blog.csdn.net/ce123_zhouwei/article/details/9050797
        r = setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flags, sizeof flags);
        if (r == -1) {
            twarn("setting TCP_NODELAY on fd %d", fd);
            close(fd);
            continue;
        }

        // IPV6设置只能IPV6连接
        if (host == NULL && ai->ai_family == AF_INET6) {
            flags = 0;
            r = setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &flags, sizeof(flags));
            if (r == -1) {
                twarn("setting IPV6_V6ONLY on fd %d", fd);
                close(fd);
                continue;
            }
        }

        // socket绑定到具体ip地址
        r = bind(fd, ai->ai_addr, ai->ai_addrlen);
        if (r == -1) {
            twarn("bind()");
            close(fd);
            continue;
        }

        // 详细模式 打印socket信息
        if (verbose) {
            char hbuf[NI_MAXHOST], pbuf[NI_MAXSERV], *h = host, *p = port;
            struct sockaddr_in addr;
            socklen_t addrlen;

            addrlen = sizeof(addr);
            r = getsockname(fd, (struct sockaddr *) &addr, &addrlen);
            if (!r) {
                r = getnameinfo((struct sockaddr *) &addr, addrlen,
                                hbuf, sizeof(hbuf),
                                pbuf, sizeof(pbuf),
                                NI_NUMERICHOST|NI_NUMERICSERV);
                if (!r) {
                    h = hbuf;
                    p = pbuf;
                }
            }
            if (ai->ai_family == AF_INET6) {
                printf("bind %d [%s]:%s\n", fd, h, p);
            } else {
                printf("bind %d %s:%s\n", fd, h, p);
            }
        }

        // 监听文件描述符并设置backlog等待队列长度为1024
        r = listen(fd, 1024);
        if (r == -1) {
            twarn("listen()");
            close(fd);
            continue;
        }

        break;
    }

    // 释放结果内存
    freeaddrinfo(airoot);

    if(ai == NULL)
        fd = -1;

    return fd;
}

static int
make_unix_socket(char *path)
{
    int fd = -1, r;
    struct stat st;
    struct sockaddr_un addr;
    const size_t maxlen = sizeof(addr.sun_path) - 1; // Reserve the last position for '\0'

    memset(&addr, 0, sizeof(struct sockaddr_un));
    addr.sun_family = AF_UNIX;
    if (strlen(path) > maxlen) {
        warnx("socket path %s is too long (%ld characters), where maximum allowed is %ld",
              path, strlen(path), maxlen);
        return -1;
    }
    strncpy(addr.sun_path, path, maxlen);

    r = stat(path, &st);
    if (r == 0) {
        if (S_ISSOCK(st.st_mode)) {
            warnx("removing existing local socket to replace it");
            r = unlink(path);
            if (r == -1) {
                twarn("unlink");
                return -1;
            }
        } else {
            twarnx("another file already exists in the given path");
            return -1;
        }
    }

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd == -1) {
        twarn("socket()");
        return -1;
    }

    r = set_nonblocking(fd);
    if (r == -1) {
        close(fd);
        return -1;
    }

    r = bind(fd, (struct sockaddr *) &addr, sizeof(struct sockaddr_un));
    if (r == -1) {
        twarn("bind()");
        close(fd);
        return -1;
    }
    if (verbose) {
        printf("bind %d %s\n", fd, path);
    }

    r = listen(fd, 1024);
    if (r == -1) {
        twarn("listen()");
        close(fd);
        return -1;
    }

    return fd;
}

int
make_server_socket(char *host, char *port)
{
#ifdef HAVE_LIBSYSTEMD
    int fd = -1, r;

    /* See if we got a listen fd from systemd. If so, all socket options etc
     * are already set, so we check that the fd is a TCP or UNIX listen socket
     * and return. */
    r = sd_listen_fds(1);
    if (r < 0) {
        twarn("sd_listen_fds");
        return -1;
    }
    if (r > 0) {
        if (r > 1) {
            twarnx("inherited more than one listen socket;"
                   " ignoring all but the first");
        }
        fd = SD_LISTEN_FDS_START;
        r = sd_is_socket_inet(fd, 0, SOCK_STREAM, 1, 0);
        if (r < 0) {
            twarn("sd_is_socket_inet");
            errno = -r;
            return -1;
        }
        if (r == 0) {
            r = sd_is_socket_unix(fd, SOCK_STREAM, 1, NULL, 0);
            if (r < 0) {
                twarn("sd_is_socket_unix");
                errno = -r;
                return -1;
            }
            if (r == 0) {
                twarnx("inherited fd is not a TCP or UNIX listening socket");
                return -1;
            }
        }
        return fd;
    }
#endif

    if (host && !strncmp(host, "unix:", 5)) {
        // 与普通的网络socket相比，不需要进行复杂的数据打包拆包，校验和计算验证，不需要走网络协议栈
        return make_unix_socket(&host[5]);
    } else {
        return make_inet_socket(host, port);
    }
}
