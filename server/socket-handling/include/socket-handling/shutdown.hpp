#pragma once

// 
// Maintainer: Athena Boose <pestpestthechicken@yahoo.com>
//
// Description:
// Cleanly sends a closing message to any added sockets before properly calling shutdown() and then close() on them.
//
// NO_TESTS

#include <cstddef>
#include <cstring>
#include <utility>
#include <unordered_map>
#include <iostream>
#include <array>
#include <sys/socket.h>
#include <socket-handling/output-buffer.hpp>
#include <socket-handling/timer.hpp>
#include <socket-handling/fd-poll.hpp>
#include <socket-handling/fatal-error.hpp>

template <std::size_t EPOLL_BUF_SIZE = 16, std::size_t CLOSE_TIME_MS = 10000>
class Shutdown {
    public:
    // Makes a best-effort attempt to shutdown the socket and ensure any messages sent to it are fully sent.
    // Assuming no errors occur, any messages already sent to the socket will be fully sent before the socket is actually closed.
    void shutdown_sock(int fd, OutputBuffer &&outbuf);
    void callback();
    int get_fd() const { return poll.fd(); }

    private:
    enum SockState {
        SendingMsg,
        WaitingZeroRead
    };
    struct ClosingSocket {
        ClosingSocket(int sock_fd, OutputBuffer &&outbuf) : sock_fd(sock_fd), outbuf(std::move(outbuf)){}
        ClosingSocket(const ClosingSocket &) = delete;
        ClosingSocket &operator=(const ClosingSocket &) = delete;
        ClosingSocket(ClosingSocket &&) = default;
        ClosingSocket &operator=(ClosingSocket &&) = default;

        Timer timer;
        int sock_fd;
        OutputBuffer outbuf;
    };

    std::unordered_map<int, ClosingSocket> map{};
    FdPoll poll;
    std::array<epoll_event, EPOLL_BUF_SIZE> event_list;
};


template <std::size_t EPOLL_BUF_SIZE, std::size_t CLOSE_TIME_MS>
void Shutdown<EPOLL_BUF_SIZE, CLOSE_TIME_MS>::shutdown_sock(int fd, OutputBuffer &&outbuf) {
    try {
        ClosingSocket sock(fd, std::move(outbuf));
        SocketStatus status = sock.outbuf.flush(fd);
        epoll_event ev{ .events = EPOLLOUT | EPOLLRDHUP | EPOLLET, .data = {.fd = fd} };
        switch (status) {
        case SocketStatus::Finished:
        case SocketStatus::Blocked:
            if (sock.outbuf.empty()) {
                ev.events = EPOLLRDHUP | EPOLLET;
                std::cerr << "Called shutdown on fd." << std::endl;
                shutdown(fd, SHUT_WR);
            }
            sock.timer.set(CLOSE_TIME_MS);
            poll.ctl(EPOLL_CTL_ADD, fd, ev);
            ev.events = EPOLLIN;
            poll.ctl(EPOLL_CTL_ADD, sock.timer.get_fd(), ev); // making the timerfd "point" to the fd of the socket we want to close; epoll will tell us how to find the proper stuff in our hashmap
            map.try_emplace(fd, std::move(sock));
            break;
        case SocketStatus::ZeroReturned:
            throw FatalError(-1, "Write returned 0 when trying to send closing message.");
        case SocketStatus::Error:
            throw FatalError(errno, "Encountered a write error while closing socket.");
        }
    }
    catch (const FatalError &e) {
        std::cerr << "Failed to shutdown socket, forceclosing. More info:\n" ;
        std::cerr << e.what() << "\n" << std::endl;
        close_except(fd);
    }
}

template <std::size_t EPOLL_BUF_SIZE, std::size_t CLOSE_TIME_MS>
void Shutdown<EPOLL_BUF_SIZE, CLOSE_TIME_MS>::callback() {
    std::span<epoll_event> events(
        poll.wait(std::span(event_list), 0)
    );
    for (const epoll_event &ev : events) {
        epoll_event temp_ev;

        if (ev.events & EPOLLRDHUP || // socket has sent us the 0-byte read
            ev.events & EPOLLERR || // socket has broken somehow
            ev.events & EPOLLHUP ||
            ev.events & EPOLLIN // timer went off
        ) {
            // exception here means the epoll this class relies on is cooked, so it's reasonable to terminate the server
            // in future I'd probably do better error handling than this but hey it shouuuld work fine so I don't care
            poll.ctl(EPOLL_CTL_DEL, ev.data.fd, temp_ev);
            poll.ctl(EPOLL_CTL_DEL, map.at(ev.data.fd).timer.get_fd(), temp_ev);
            close_except(ev.data.fd);
            // timerfd destructs automatically, thank you RAII
            map.erase(ev.data.fd);
            continue;
        }

        OutputBuffer &outbuf = map.at(ev.data.fd).outbuf;
        SocketStatus status = outbuf.flush(ev.data.fd);
        switch (status) {
        case SocketStatus::Finished:
        case SocketStatus::Blocked:
            if (outbuf.empty()) {
                temp_ev.events = EPOLLRDHUP | EPOLLET;
                temp_ev.data.fd = ev.data.fd;
                poll.ctl(EPOLL_CTL_MOD, ev.data.fd, temp_ev);
                std::cerr << "Called shutdown() on fd" << std::endl;
                shutdown(ev.data.fd, SHUT_WR);
            }
            break;
        case SocketStatus::ZeroReturned:
        case SocketStatus::Error:
            poll.ctl(EPOLL_CTL_DEL, ev.data.fd, temp_ev);
            poll.ctl(EPOLL_CTL_DEL, map.at(ev.data.fd).timer.get_fd(), temp_ev);
            close_except(ev.data.fd);
            // timerfd destructs automatically, thank you RAII
            map.erase(ev.data.fd);
            break;
        }
    }
}
