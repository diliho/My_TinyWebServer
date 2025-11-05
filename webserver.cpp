#include "webserver.h"
#include <malloc.h>

WebServer::WebServer()
{
    // http_conn类对象
    users = new http_conn[MAX_FD];

    // root文件夹路径
    char server_path[200];
    getcwd(server_path, 200);
    char root[6] = "/root";
    m_root = (char *)malloc(strlen(server_path) + strlen(root) + 1);
    strcpy(m_root, server_path);
    strcat(m_root, root);

    // 定时器
    users_timer = new client_data[MAX_FD];

    // uring初始化
    if (io_uring_queue_init(RING_ENTRIES, &m_uring, 0) < 0)
    {
        LOG_ERROR("%s", "io_uring_queue_init failure");
        exit(EXIT_FAILURE);
    }
    m_use_liburing = false;
}

WebServer::~WebServer()
{
    close(m_epollfd);
    close(m_listenfd);
    close(m_pipefd[1]);
    close(m_pipefd[0]);
    delete[] users;
    delete[] users_timer;
    delete m_pool;

    io_uring_queue_exit(&m_uring);
}

void WebServer::init(int port, string user, string passWord, string databaseName, int log_write,
                     int opt_linger, int trigmode, int sql_num, int thread_num, int close_log, int actor_model)
{
    m_port = port;
    m_user = user;
    m_passWord = passWord;
    m_databaseName = databaseName;
    m_sql_num = sql_num;
    m_thread_num = thread_num;
    m_log_write = log_write;
    m_OPT_LINGER = opt_linger;
    m_TRIGMode = trigmode;
    m_close_log = close_log;
    m_actormodel = actor_model;
}

void WebServer::trig_mode()
{
    // LT + LT
    if (0 == m_TRIGMode)
    {
        m_LISTENTrigmode = 0;
        m_CONNTrigmode = 0;
    }
    // LT + ET
    else if (1 == m_TRIGMode)
    {
        m_LISTENTrigmode = 0;
        m_CONNTrigmode = 1;
    }
    // ET + LT
    else if (2 == m_TRIGMode)
    {
        m_LISTENTrigmode = 1;
        m_CONNTrigmode = 0;
    }
    // ET + ET
    else if (3 == m_TRIGMode)
    {
        m_LISTENTrigmode = 1;
        m_CONNTrigmode = 1;
    }
}

void WebServer::log_write()
{
    if (0 == m_close_log)
    {
        // 初始化日志
        if (1 == m_log_write)
            Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 800);
        else
            Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 0);
    }
}

void WebServer::sql_pool()
{
    // 初始化数据库连接池
    m_connPool = connection_pool::GetInstance();
    m_connPool->init("localhost", m_user, m_passWord, m_databaseName, 3306, m_sql_num, m_close_log);

    // 初始化数据库读取表
    users->initmysql_result(m_connPool);
}

void WebServer::thread_pool()
{
    // 线程池
    m_pool = new threadpool<http_conn>(m_actormodel, m_connPool, m_thread_num);
}

void WebServer::eventListen()
{
    // 网络编程基础步骤
    m_listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(m_listenfd >= 0);

    // 优雅关闭连接
    if (0 == m_OPT_LINGER)
    {
        struct linger tmp = {0, 1};
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }
    else if (1 == m_OPT_LINGER)
    {
        struct linger tmp = {1, 1};
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }

    int ret = 0;
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(m_port);

    int flag = 1;
    setsockopt(m_listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
    ret = bind(m_listenfd, (struct sockaddr *)&address, sizeof(address));
    assert(ret >= 0);
    ret = listen(m_listenfd, 5);
    assert(ret >= 0);

    utils.init(TIMESLOT);

    // Proactor模式下使用liburing进行异步accept
    if (m_actormodel == 0)
    {
        m_use_liburing = true;
        submit_async_accept();
    }
    else
    { // epoll创建内核事件表
        epoll_event events[MAX_EVENT_NUMBER];
        m_epollfd = epoll_create(5);
        assert(m_epollfd != -1);

        utils.addfd(m_epollfd, m_listenfd, false, m_LISTENTrigmode);
        http_conn::m_epollfd = m_epollfd;

        ret = socketpair(PF_UNIX, SOCK_STREAM, 0, m_pipefd);
        assert(ret != -1);
        utils.setnonblocking(m_pipefd[1]);
        utils.addfd(m_epollfd, m_pipefd[0], false, 0);

        utils.addsig(SIGPIPE, SIG_IGN);
        utils.addsig(SIGALRM, utils.sig_handler, false);
        utils.addsig(SIGTERM, utils.sig_handler, false);

        alarm(TIMESLOT);

        // 工具类,信号和描述符基础操作
        Utils::u_pipefd = m_pipefd;
        Utils::u_epollfd = m_epollfd;
    }
}

void WebServer::timer(int connfd, struct sockaddr_in client_address)
{
    users[connfd].init(connfd, client_address, m_root, m_CONNTrigmode, m_close_log, m_user, m_passWord, m_databaseName);

    // 初始化client_data数据
    // 创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
    users_timer[connfd].address = client_address;
    users_timer[connfd].sockfd = connfd;
    util_timer *timer = new util_timer;
    timer->user_data = &users_timer[connfd];
    timer->cb_func = cb_func;
    time_t cur = time(NULL);
    timer->expire = cur + 3 * TIMESLOT;
    users_timer[connfd].timer = timer;
    utils.m_timer_lst.add_timer(timer);
}

// 若有数据传输，则将定时器往后延迟3个单位
// 并对新的定时器在链表上的位置进行调整
void WebServer::adjust_timer(util_timer *timer)
{
    time_t cur = time(NULL);
    timer->expire = cur + 3 * TIMESLOT;
    utils.m_timer_lst.adjust_timer(timer);

    LOG_INFO("%s", "adjust timer once");
}

void WebServer::deal_timer(util_timer *timer, int sockfd)
{
    timer->cb_func(&users_timer[sockfd]);
    if (timer)
    {
        utils.m_timer_lst.del_timer(timer);
    }

    LOG_INFO("close fd %d", users_timer[sockfd].sockfd);
}

bool WebServer::dealclientdata()
{
    struct sockaddr_in client_address;
    socklen_t client_addrlength = sizeof(client_address);
    if (0 == m_LISTENTrigmode) // LT
    {
        int connfd = accept(m_listenfd, (struct sockaddr *)&client_address, &client_addrlength);
        if (connfd < 0)
        {
            LOG_ERROR("%s:errno is:%d", "accept error", errno);
            return false;
        }
        if (http_conn::m_user_count >= MAX_FD)
        {
            utils.show_error(connfd, "Internal server busy");
            LOG_ERROR("%s", "Internal server busy");
            return false;
        }
        timer(connfd, client_address);
    }

    else // ET
    {
        while (1)
        {
            int connfd = accept(m_listenfd, (struct sockaddr *)&client_address, &client_addrlength);
            if (connfd < 0)
            {
                LOG_ERROR("%s:errno is:%d", "accept error", errno);
                break;
            }
            if (http_conn::m_user_count >= MAX_FD)
            {
                utils.show_error(connfd, "Internal server busy");
                LOG_ERROR("%s", "Internal server busy");
                break;
            }
            timer(connfd, client_address);
        }
        return false;
    }
    return true;
}

bool WebServer::dealwithsignal(bool &timeout, bool &stop_server)
{
    int ret = 0;
    int sig;
    char signals[1024];
    ret = recv(m_pipefd[0], signals, sizeof(signals), 0);
    if (ret == -1)
    {
        return false;
    }
    else if (ret == 0)
    {
        return false;
    }
    else
    {
        for (int i = 0; i < ret; ++i)
        {
            switch (signals[i])
            {
            case SIGALRM:
            {
                timeout = true;
                break;
            }
            case SIGTERM:
            {
                stop_server = true;
                break;
            }
            }
        }
    }
    return true;
}

void WebServer::dealwithread(int sockfd)
{
    util_timer *timer = users_timer[sockfd].timer;

    if (1 == m_actormodel) // proactor
    {
        if (timer)
        {
            adjust_timer(timer);
        }

        // 若监测到读事件，将该事件放入请求队列
        m_pool->append(users + sockfd, 0);

        while (true)
        {
            if (1 == users[sockfd].improv)
            {
                if (1 == users[sockfd].timer_flag)
                {
                    deal_timer(timer, sockfd);
                    users[sockfd].timer_flag = 0;
                }
                users[sockfd].improv = 0;
                break;
            }
        }
    }
    else // reactor
    {

        if (users[sockfd].read_once())
        {
            LOG_INFO("deal with the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));

            // 若监测到读事件，将该事件放入请求队列
            m_pool->append_p(users + sockfd);

            if (timer)
            {
                adjust_timer(timer);
            }
        }
        else
        {
            deal_timer(timer, sockfd);
        }
    }
}

void WebServer::dealwithwrite(int sockfd)
{
    util_timer *timer = users_timer[sockfd].timer;
    // reactor
    if (1 == m_actormodel)
    {
        if (timer)
        {
            adjust_timer(timer);
        }

        m_pool->append(users + sockfd, 1);

        while (true)
        {
            if (1 == users[sockfd].improv)
            {
                if (1 == users[sockfd].timer_flag)
                {
                    deal_timer(timer, sockfd);
                    users[sockfd].timer_flag = 0;
                }
                users[sockfd].improv = 0;
                break;
            }
        }
    }
}

void WebServer::eventLoop()
{
    bool timeout = false;
    bool stop_server = false;

    while (!stop_server)
    { // Proactor模式使用io_uring
        if (m_use_liburing && m_actormodel == 0)
        {
            struct io_uring_cqe *cqe;

            int ret = io_uring_wait_cqe(&m_uring, &cqe);
            if (ret < 0)
            {
                LOG_ERROR("io_uring_wait_cqe failure, ret: %d", ret);
                break;
            }
            struct conn_info *ci = (struct conn_info *)io_uring_cqe_get_data(cqe);
            if (!ci)
            {
                LOG_ERROR("Invalid conn_info in CQE");
                io_uring_cqe_seen(&m_uring, cqe);
                continue;
            }
            if (ci->fd == m_listenfd && ci->op_type == OP_ACCEPT)
            {
                handle_async_accept(cqe);
            }
            else if (ci->op_type == OP_READ)
            {
                handle_async_read(cqe);
            }

            io_uring_cqe_seen(&m_uring, cqe);
        }
        else
        {
            int number = epoll_wait(m_epollfd, events, MAX_EVENT_NUMBER, -1);
            if (number < 0 && errno != EINTR)
            {
                LOG_ERROR("%s", "epoll failure");
                break;
            }

            for (int i = 0; i < number; i++)
            {
                int sockfd = events[i].data.fd;

                // 处理新到的客户连接
                if (sockfd == m_listenfd)
                {
                    bool flag = dealclientdata();
                    if (false == flag)
                        continue;
                }
                else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
                {
                    // 服务器端关闭连接，移除对应的定时器
                    util_timer *timer = users_timer[sockfd].timer;
                    deal_timer(timer, sockfd);
                }
                // 处理信号
                else if ((sockfd == m_pipefd[0]) && (events[i].events & EPOLLIN))
                {
                    bool flag = dealwithsignal(timeout, stop_server);
                    if (false == flag)
                        LOG_ERROR("%s", "dealclientdata failure");
                }
                // 处理客户连接上接收到的数据
                else if (events[i].events & EPOLLIN)
                {
                    dealwithread(sockfd);
                }
                else if (events[i].events & EPOLLOUT)
                {
                    dealwithwrite(sockfd);
                }
            }
        }
        if (timeout)
        {
            utils.timer_handler();

            LOG_INFO("%s", "timer tick");

            timeout = false;
        }
    }
}

/*从cqe->user_data获取conn_info。
通过cqe->res获取新连接的sockfd（成功时res > 0）。
释放conn_info的内存，避免泄漏。*/
void WebServer::handle_async_accept(struct io_uring_cqe *cqe)
{
    struct conn_info *ci = (struct conn_info *)io_uring_cqe_get_data(cqe);
    if (!ci)
    {
        LOG_ERROR("get user_data failed");
        return;
    }

    if (cqe->res < 0)
    {
        LOG_ERROR("async accept failed: %d", cqe->res);
        free(ci);
        return;
    }
    int new_connfd = cqe->res;
    // 为新连接创建并初始化一个http_conn实例
    users[new_connfd].init(
        new_connfd,
        ci->client_addr,
        m_root,
        m_CONNTrigmode,
        m_close_log,
        m_user,
        m_passWord,
        m_databaseName);

    free(ci);

    submit_async_accept();
}
/*从cqe->user_data获取conn_info；
通过cqe->res获取读取的字节数（>0为成功）；
更新http_conn的m_read_idx，并触发后续处理（如解析 HTTP 请求）；
释放conn_info内存，避免泄漏。*/
void WebServer::handle_async_read(struct io_uring_cqe *cqe)
{
    struct conn_info *ci = (struct conn_info *)io_uring_cqe_get_data(cqe);
    int sockfd = ci->fd;

    int res = cqe->res;
    if (res <= 0)
    {
        LOG_ERROR("Read failed, fd: %d, res: %zd", sockfd, res);
        users[sockfd].close_conn();
    }
    else
    {
        if (!m_pool->append(&users[sockfd], 0))
        {
            LOG_ERROR("Read buffer overflow, fd: %d", sockfd);
        }
    }
}

void WebServer::submit_async_accept()
{
    if (!m_use_liburing)
    {
        LOG_ERROR("async accept is invalued");
        return;
    }

    struct conn_info *ci = (struct conn_info *)malloc(sizeof(struct conn_info));
    if (!ci)
    {
        LOG_ERROR("malloc conn_info failed");
        return;
    }

    ci->fd = m_listenfd;
    ci->op_type = OP_ACCEPT;
    ci->client_len = sizeof(ci->client_addr);

    struct io_uring_sqe *sqe = io_uring_get_sqe(&m_uring);
    if (!sqe)
    {
        LOG_ERROR("io_uring_get_sqe failed (queue full)");
        free(ci);
        return;
    }

    io_uring_prep_accept(
        sqe,
        m_listenfd,
        (struct sockaddr *)&(ci->client_addr),
        &(ci->client_len),
        0);

    io_uring_sqe_set_data(sqe, ci);

    int ret = io_uring_submit(&m_uring);
    if (ret <= 0)
    {
        LOG_ERROR("io_uring_submit failed: %d, errno: %d", ret, errno);
        free(ci);
        return;
    }
}