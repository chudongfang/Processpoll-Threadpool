/**
 * filename: processpool.h
 * created by cdf ^_^~~
 */

#ifndef PROCESSPOOL_H
#define PROCESSPOOL_H

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <iosfwd>
#include <iostream>

/*描述一个子进程的类*/
class process{
public:
    process() : m_pid(-1){};

public:
    pid_t m_pid;
    int m_pipefd[2];
};



template <typename  T >
class processpool{


private:
    //构造函数
    processpool(int listenfd,int process_number = 8);




public:
    //创建进程池
    static processpool< T >* create(int listenfd, int process_number = 8)
    {
        if(!m_instance)
        {
            m_instance = new processpool< T >(listenfd , process_number);
            return m_instance;
        }
    }

    //析构函数，还原线程池
    ~processpool()
    {
        delete [] m_instance;
    }

    //运行线程
    void run();

private:
    void setup_sig_pipe();
    void run_parent();
    void run_child();
private:
    static const int MAX_PROCESS_NUMBER = 16;//进程池最大子进程数量
    static const int USER_PER_PROCESS = 65536;//每个子进程最多能处理客户端数量
    static const int MAX_EVENT_NUMBER = 10000;//epoll最多能处理的事件数
    int m_process_number;//进程池中进程的数量
    int m_idx;//进程在池中的序号，从0开始
    int m_epollfd;//每个进程都有一个epoll内核事件表，用m_epollfd标识
    int m_listenfd;//监听socket
    bool m_stop;//子进程通过m_stop决定是否停止运行

    process* m_sub_process; //保存所有进程的静态信息
    static processpool< T >* m_instance;//进程池实例
};

//初始化
template < typename T >
processpool< T >* processpool< T >::m_instance =  NULL;

//父进程与子进程间的管道
static int sig_pipefd[2];

//设置文件为非阻塞 fcntl函数实现
static int setnonblocking(int fd)
{
    int old_option = fcntl(fd,F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL,new_option);
    return old_option;
}

//将fd放入epoll内核时间表
static void addfd(int epollfd, int fd)
{
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLET;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

//从epollfd标记的epoll内核事件表中删除fd上所有注册事件
static void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL,fd,0);
    close(fd);
}


//信号处理函数
static void sig_handler(int sig)
{
    int save_errno = errno;
    int msg = sig;
    send(sig_pipefd[1],(char *)&msg, 1, 0);
    errno = save_errno;
}

//设置信号处理函数
static void addsig(int sig, void(handler)(int), bool restart = true)
{
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;
    if(restart)
    {
        sa.sa_flags |= SA_RESTART;
    }
    sigfillset(&sa.sa_mask);
    assert( sigaction( sig, &sa,NULL) != -1 );
}


template < typename  T >
processpool< T >::processpool(int listenfd, int process_number)
        :m_listenfd(listenfd),m_process_number(process_number),m_idx(-1),m_stop(false)
{
    assert( (process_number>0) && (process_number<=MAX_PROCESS_NUMBER) );
    m_sub_process = new process[process_number];
    assert( m_sub_process );

    //创建子进程，并建立它们和父进程之间的管道
    for (int i = 0; i < process_number; ++i)
    {
        //将父进程和进程信号量关联
        int ret = socketpair(PF_UNIX, SOCK_STREAM, 0, m_sub_process[i].m_pipefd);

        //判断关联是否成功
        assert( ret==0 );

        //fork创建子进程
        m_sub_process[i].m_pid = fork();

        //判断创建进程是否成功
        assert(m_sub_process[i].m_pid>=0);

        if(m_sub_process[i].m_pid>0)//父进程
        {
            close(m_sub_process[i].m_pipefd[1]);
            continue;
        }
        else//子进程
        {
            close(m_sub_process[i].m_pipefd[0]);
            m_idx = i;  // 确定子进程在进程池中的标号
            break;
        }
    }
}



//统一事件源
template < typename T >
void processpool< T >::setup_sig_pipe()
{
    m_epollfd  = epoll_create(5);
    assert(m_epollfd!=-1);
    int ret = socketpair(PF_UNIX, SOCK_STREAM, 0, sig_pipefd);//
    assert( ret!=1 );

    //设置为非阻塞
    setnonblocking(sig_pipefd[1]);

    //添加到进程epoll内核事件表
    addfd(m_epollfd,sig_pipefd[0]);

    //设置处理信号函数
    addsig(SIGCHLD, sig_handler);
    addsig(SIGTERM, sig_handler);
    addsig(SIGINT, sig_handler);
    addsig(SIGPIPE, sig_handler);
}




template < typename T >
void processpool< T >::run()
{
    //利用m_idx判断该进程是父进程还是子进程
    if(m_idx != -1)
    {
        run_child();
        return ;
    }
    run_parent();
}



/**
 * name :run_child
 * 1.this function will listen pipefd,it will get the request from father
 *   and accept the request
 * 2.it will call process function to deal with the socket
 * @tparam T
 */
template < typename T >
void processpool<T>::run_child()
{
    setup_sig_pipe();

    /*每个子进程通过自己的编号找到对应的父进程通信管道*/
    int pipefd =m_sub_process[m_idx].m_pipefd[ 1 ];

    /*子进程监听管道文件描述符*/
    addfd(m_epollfd, pipefd);

    epoll_event events[MAX_EVENT_NUMBER];

    T* users = new T [USER_PER_PROCESS ];
    assert(users);
    int number = 0;
    int ret = -1;
    /*判断进程池是否停止*/
    while(! m_stop)
    {
        //从epoll获取事件，events存储事件
        number = epoll_wait( m_epollfd, events, MAX_EVENT_NUMBER, -1);
        //judge epoll failure
        if((number < 0) && ( errno != EINTR))
        {
            std::cout<<"Child$: epoll failure"<<std::endl;
        }

        for(int i = 0; i < number; i++)
        {
            int sockfd = events[i].data.fd;

            //如果是父进程管道发来的用户链接
            if((sockfd == pipefd ) && (events[i].events & EPOLLIN))
            {
                std::cout<<"Child$: I get a message from father."<<std::endl;
                int client  = 0;
                /*从父子进程之间的管道读取数据，并将结果保存在变量client中，如果读取成功，则表示有新客户到来*/
                ret = recv(sockfd, (char *)&client, sizeof(client) , 0);

                if((( ret<0 ) && (errno != EAGAIN ) ) || ret == 0 )
                {
                    continue;//无数据
                }
                else
                {
                    std::cout<<"Child$: there is a new client."<<std::endl;

                    //与用户链接
                    struct sockaddr_in client_address;
                    socklen_t  client_addrlength = sizeof(client_address);

                    //父子进程共享一个m_listenfd，当父进程向子进程发出读信号时，子进程接受连接
                    int connfd = accept(m_listenfd ,(struct sockaddr*)&client_address, &client_addrlength);
                    if (connfd < 0)
                    {
                        std::cout<<"Child$: errno is "<<errno<<std::endl;
                        continue;
                    }
                    addfd( m_epollfd, connfd);
                    /*实现init方法， 以初始化一个客户连接*/
                    users[connfd].init(m_epollfd, connfd, client_address);
                }

            }
                //处理信号
            else if(( sockfd == sig_pipefd[0]) && ( events[i].events & EPOLLIN))
            {
                char signals[1024];
                ret = recv( sig_pipefd[0], signals, sizeof(signals), 0);
                if(ret <= 0)
                {
                    continue;
                }
                else
                {


                    for(int i = 0; i < ret; ++i)
                    {
                        switch (signals[i])
                        {
                            case SIGCHLD:
                            {
                                pid_t  pid;
                                int stat;
                                //阻塞退出
                                while((pid == waitpid(-1, &stat, WNOHANG)) > 0)
                                {
                                    continue;
                                }
                                break;
                            }
                            case SIGTERM:
                            case SIGINT:
                            {
                                //收到中断信号  如Ctrl + c
                                m_stop = true;
                                break;
                            }
                            default:
                            {
                                break;
                            }
                        }
                    }


                }
            }
                /*其必然是客户请求调用process方法处理*/
            else if(events[i].events & EPOLLIN)
            {
                std::cout<<"Child$: I get a send from client."<<std::endl;
                //process函数应该实行I/O操作，利用recv/send函数，读数据，并处理
                users[sockfd].process();
            }
            else
            {
                continue;
            }
        }
    }

    delete [] users;
    users = NULL;
    close( pipefd );
    close(m_epollfd);
}





/**
 * name : run_parent
 * 1.this function will listen m_listenfd,and deal with socket request.
 *   the request will be send to children process.
 * 2.this function will manage the children process,for example kill them
 * @tparam T
 */

template < typename T>
void processpool <T>::run_parent()
{
    setup_sig_pipe();

    /*父进程监听m_listenfd*/
    addfd( m_epollfd, m_listenfd);

    epoll_event events[MAX_EVENT_NUMBER];
    int sub_process_counter = 0;
    int new_conn = 1;
    int number = 0;
    int ret = -1;
    while( ! m_stop )
    {
        number = epoll_wait( m_epollfd, events, MAX_EVENT_NUMBER, -1);
        if((number < 0)&& (errno != EINTR ) )
        {
            std::cout<<"Father$: epoll failure\n"<<std::endl;
            break;
        }

        for(int i = 0; i < number; i++)
        {
            int sockfd = events[i].data.fd;
            /*处理客户事件*/
            if( sockfd == m_listenfd)
            {
                /*如果有连接到来，就用 Round Robin方式将其分配个一个子进程处理*/
                int cur = sub_process_counter;
                do
                {
                    if(m_sub_process[cur].m_pid != -1)
                    {
                        break;
                    }
                    cur = (cur+1)%m_process_number;
                }
                while(cur != sub_process_counter );


                if( m_sub_process[cur].m_pid == 1)
                {
                    m_stop = true;
                    break;
                }

                sub_process_counter = (cur+1)%m_process_number;

                send( m_sub_process[cur].m_pipefd[0], (char*)&new_conn, sizeof( new_conn ), 0 );
                std::cout<<"Father$: I get a request from client and "
                           "i will send request to child "<<cur<<std::endl;
            }
                //处理信号
            else if(( sockfd == sig_pipefd[0] ) && (events[i].events & EPOLLIN))
            {
                int sig;
                char signals[1024];
                //从子进程读信号
                ret  = recv(sig_pipefd[0], signals ,sizeof( signals ), 0);
                if(ret <= 0)
                {
                    //读不到或读取失败直接略过
                    continue;
                }
                else
                {
                    for(int i = 0; i < ret; ++i)
                    {
                        //根据信号种类进行处理
                        switch ( signals[i] )
                        {
                            case SIGCHLD:
                            {
                                pid_t pid;
                                int stat;
                                //waitpid 当其参数为-1时与wait功能相同，如果没有子进程退出则返回0，如果有返回子进程ID
                                while((pid = waitpid( -1, &stat, WNOHANG)) > 0)
                                {
                                    for(int k = 0;k < m_process_number; ++k)
                                    {
                                        /*如果进程池中第i个子进程退出了，则主进程关闭相应的通道，并设置相应的m_pid为-1 */
                                        if(m_sub_process[k].m_pid == pid)
                                        {
                                            std::cout<<"Father$:  child "<<k<<" join"<<std::endl;
                                            close(m_sub_process[k].m_pipefd[0]);
                                            m_sub_process[k].m_pid = -1;
                                        }
                                    }
                                }
                                /*如果所有子进程退出，父进程也退出*/
                                m_stop = true;
                                for(int k = 0; k<m_process_number; ++k)
                                {
                                    if(m_sub_process[k].m_pid != -1)
                                    {
                                        m_stop = false;
                                    }
                                }
                                break;
                            }
                            case SIGTERM:
                                /*父进程收到终止信号，则直接退出*/
                            case SIGINT:
                            {
                                std::cout<<"Father$:I will kill all children process now "<<std::endl;
                                for(int i = 0; i < m_process_number; ++i)
                                {
                                    //如果子进程没有结束就终止子进程
                                    int pid = m_sub_process[i].m_pid;
                                    if(pid != -1)
                                    {
                                        kill(pid , SIGTERM);
                                    }
                                }
                                break;
                            }
                            default:
                            {
                                break;
                            }
                        }
                    }
                }
            }
            else
            {
                continue;
            }
        }
    }
    close(m_epollfd);
}


#endif



