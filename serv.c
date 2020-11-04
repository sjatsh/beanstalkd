#include "dat.h"
#include <stdint.h>
#include <stdlib.h>
#include <sys/socket.h>

struct Server srv = {
    .port = Portdef,
    .wal = {
        .filesize = Filesizedef,
        .wantsync = 1,
        .syncrate = DEFAULT_FSYNC_MS * 1000000,
    },
};

// srv_acquire_wal tries to lock the wal dir specified by s->wal and
// replay entries from it to initialize the s state with jobs.
// On errors it exits from the program.
void srv_acquire_wal(Server *s) {
    if (s->wal.use) {
        // We want to make sure that only one beanstalkd tries
        // to use the wal directory at a time. So acquire a lock
        // now and never release it.
        if (!waldirlock(&s->wal)) {
            twarnx("failed to lock wal dir %s", s->wal.dir);
            exit(10);
        }

        Job list = {.prev=NULL, .next=NULL};
        list.prev = list.next = &list;
        // 初始化wal持久化对象
        walinit(&s->wal, &list);
        // 从文件恢复job
        int ok = prot_replay(s, &list);
        if (!ok) {
            twarnx("failed to replay log");
            exit(1);
        }
    }
}

void
srvserve(Server *s)
{
    Socket *sock;

    // 初始化epoll或者kqueue等异步io
    if (sockinit() == -1) {
        twarnx("sockinit");
        exit(1);
    }

    s->sock.x = s;
    s->sock.f = (Handle)srvaccept; // accept回调
    s->conns.less = conn_less;
    s->conns.setpos = conn_setpos;

    // server fd加入epoll并注册读事件
    if (sockwant(&s->sock, 'r') == -1) {
        twarn("sockwant");
        exit(2);
    }

    for (;;) {
        // 获取最近的一个job超时时间
        int64 period = prottick(s);

        // 阻塞在这里直到获取下一个客户端读写事件或者等到超时时间到来
        int rw = socknext(&sock, period);
        if (rw == -1) {
            twarnx("socknext");
            exit(1);
        }

        // 如果是读写事件 使用注册的accept handler处理客户端请求
        if (rw) {
            // 使用注册的回调处理请求
            sock->f(sock->x, rw);
        }
    }
}


void
srvaccept(Server *s, int ev)
{
    h_accept(s->sock.fd, ev, s);
}
