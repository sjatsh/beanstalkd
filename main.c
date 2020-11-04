#include "dat.h"
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pwd.h>
#include <fcntl.h>

static void
su(const char *user) 
{
    errno = 0;
    struct passwd *pwent = getpwnam(user);
    if (errno) {
        twarn("getpwnam(\"%s\")", user);
        exit(32);
    }
    if (!pwent) {
        twarnx("getpwnam(\"%s\"): no such user", user);
        exit(33);
    }

    int r = setgid(pwent->pw_gid);
    if (r == -1) {
        twarn("setgid(%d \"%s\")", pwent->pw_gid, user);
        exit(34);
    }

    r = setuid(pwent->pw_uid);
    if (r == -1) {
        twarn("setuid(%d \"%s\")", pwent->pw_uid, user);
        exit(34);
    }
}

static void
handle_sigterm_pid1()
{
    exit(143);
}

static void
set_sig_handlers()
{
    struct sigaction sa;

    sa.sa_handler = SIG_IGN; // 忽略信号处理程序
    sa.sa_flags = 0;
    // 主要作用就是将信号集 sa.sa_mask 初始化为空
    int r = sigemptyset(&sa.sa_mask);
    if (r == -1) {
        twarn("sigemptyset()");
        exit(111);
    }

    // 监听SIGPIPE信号并且忽略不处理
    // 这里为了防止client意外断开连接 服务端继续往socket写后导致程序异常退出
    r = sigaction(SIGPIPE, &sa, 0);
    if (r == -1) {
        twarn("sigaction(SIGPIPE)");
        exit(111);
    }

    // 设置handler并且监听SIGUSR1用户自定义信号
    sa.sa_handler = enter_drain_mode;
    r = sigaction(SIGUSR1, &sa, 0);
    if (r == -1) {
        twarn("sigaction(SIGUSR1)");
        exit(111);
    }

    // Workaround for running the server with pid=1 in Docker.
    // Handle SIGTERM so the server is killed immediately and
    // not after 10 seconds timeout. See issue #527.
    if (getpid() == 1) {
        sa.sa_handler = handle_sigterm_pid1;
        r = sigaction(SIGTERM, &sa, 0);
        if (r == -1) {
            twarn("sigaction(SIGTERM)");
            exit(111);
        }
    }
}

int
main(int argc, char **argv)
{
    UNUSED_PARAMETER(argc);

    progname = argv[0];
    setlinebuf(stdout);

    // 解析命令行参数
    optparse(&srv, argv+1);

    if (verbose) {
        printf("pid %d\n", getpid());
    }

    // 创建文件描述符
    int r = make_server_socket(srv.addr, srv.port);
    if (r == -1) {
        twarnx("make_server_socket()");
        exit(111);
    }

    // 赋值server端文件描述符
    srv.sock.fd = r;

    // tube链表初始化 并且初始化default tube
    prot_init();

    // 使用设置用户启动
    if (srv.user)
        su(srv.user);

    // 监听退出信号 退出前 设置 drain_mode=1并且不再接受新的请求
    set_sig_handlers();

    // 锁定持久化目录读取binlog并初始化tube和job
    srv_acquire_wal(&srv);

    // 启动服务并监听服务
    srvserve(&srv);
    exit(0);
}
