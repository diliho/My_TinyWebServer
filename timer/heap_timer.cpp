#include "heap_timer.h"
#include "../http/http_conn.h"

heap_timer_lst::heap_timer_lst(int capacity) : capacity(capacity)
{
    heap.reserve(capacity);
}

heap_timer_lst::~heap_timer_lst()
{
    for (int i = 0; i < heap.size(); ++i)
    {
        if (heap[i])
        {
            delete heap[i];
        }
    }
}

void heap_timer_lst::swap(util_timer *&a, util_timer *&b)
{
    int temp = a->index;
    a->index = b->index;
    b->index = temp;

    util_timer *tmp = a;
    a = b;
    b = tmp;
}

void heap_timer_lst::percolate_up(int hole)
{
    // 向上调整
    int parent = (hole - 1) / 2;
    while (hole > 0)
    {
        if (heap[parent]->expire <= heap[hole]->expire)
            break;

        swap(heap[parent], heap[hole]);
        hole = parent;
        parent = (hole - 1) / 2;
    }
}

void heap_timer_lst::percolate_down(int hole)
{
    // 向下调整
    int child = 2 * hole + 1;
    while (child < heap.size()) // 左子节点存在
    {                           // 选在最小的子节点
        if (child + 1 < heap.size() && heap[child + 1]->expire < heap[child]->expire)
            child++;

        if (heap[hole]->expire <= heap[child]->expire)
            break;

        swap(heap[hole], heap[child]);
        hole = child;
        child = 2 * hole + 1;
    }
}

void heap_timer_lst::resize()
{
    // 容量翻倍
    capacity *= 2;
    heap.reserve(capacity);
}

void heap_timer_lst::add_timer(util_timer *timer)
{
    if (!timer)
        return;

    if (heap.size() >= capacity)
        resize();

    // 插入到堆尾并上浮
    timer->index = heap.size();
    heap.push_back(timer);
    percolate_up(heap.size() - 1);
}

void heap_timer_lst::adjust_timer(util_timer *timer)
{
    if (!timer)
        return;
    int adj_index = timer->index;
    if (adj_index < 0 || adj_index >= heap.size())
        return;
    if (adj_index == 0)
    {
        percolate_down(adj_index);
        return;
    }
    else
    {
        int parent = (adj_index - 1) / 2;
        if (timer->expire < heap[parent]->expire)
        {
            percolate_up(adj_index);
            return;
        }
        else
        {
            percolate_down(adj_index);
            return;
        }
    }
}

void heap_timer_lst::del_timer(util_timer *timer)
{
    if (!timer || heap.size() == 0)
        return;
    int del_index = timer->index;
    // 情况1：待删除节点是堆尾，直接删除
    if (del_index == heap.size() - 1)
    {
        delete heap[del_index];
        heap.pop_back();
        return;
    }
    // 情况2：非堆尾
    // 交换堆数组中「待删除位置」和「堆尾位置」的元素
    swap(heap[del_index], heap[heap.size() - 1]);
    delete heap[heap.size() - 1];
    heap.pop_back();

    if (del_index == 0)
    {
        percolate_down(del_index);
        return;
    }
    else
    {
        int parent = (del_index - 1) / 2;
        if (heap[del_index]->expire < heap[parent]->expire)
        {
            percolate_up(del_index);
            return;
        }
        else
        {
            percolate_down(del_index);
            return;
        }
    }
}

void heap_timer_lst::tick()
{
    if (heap.size() == 0)
        return;

    time_t cur = time(NULL);

    // 处理所有过期的定时器
    while (heap.size() > 0)
    {
        util_timer *timer = heap[0];

        if (!timer)
            break;

        if (cur < timer->expire)
            break;

        // 执行回调函数
        if (timer->cb_func)
            timer->cb_func(timer->user_data);

        // 删除根节点
        del_timer(timer);
    }
}

void Utils::init(int timeslot)
{
    m_TIMESLOT = timeslot;
}

int Utils::setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

void Utils::addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else
        event.events = EPOLLIN | EPOLLRDHUP;

    if (one_shot)
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

void Utils::sig_handler(int sig)
{
    int save_errno = errno;
    int msg = sig;
    send(u_pipefd[1], (char *)&msg, 1, 0);
    errno = save_errno;
}

void Utils::addsig(int sig, void(handler)(int), bool restart)
{
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;
    if (restart)
        sa.sa_flags |= SA_RESTART;
    sigfillset(&sa.sa_mask);
    assert(sigaction(sig, &sa, NULL) != -1);
}

void Utils::timer_handler()
{
    m_timer_lst.tick();
    alarm(m_TIMESLOT);
}

void Utils::show_error(int connfd, const char *info)
{
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

int *Utils::u_pipefd = 0;
int Utils::u_epollfd = 0;

void cb_func(client_data *user_data)
{
    epoll_ctl(Utils::u_epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    assert(user_data);
    close(user_data->sockfd);
    http_conn::m_user_count--;
}