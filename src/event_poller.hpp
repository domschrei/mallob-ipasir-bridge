
#pragma once

#include "timer.hpp"
#include <set>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <sys/inotify.h>
#include <iostream>
#include <unistd.h>
#include <atomic>
#include <shared_mutex>
#include <assert.h>
#include <thread>
#include <mutex>

class EventPoller {

public:
    struct PollState {
        int idx {0};
    };

private:
    bool _initialized = false;
    int _fd_inotify;
    int _fd_inotify_watcher;

    std::mutex _poll_mutex;
    std::condition_variable _poll_cond_var;
    std::vector<std::string> _incoming_event_files;

    std::thread _bg_thread;

public:
    EventPoller() {}
    EventPoller(const std::string& watchedDir) {

         _fd_inotify = inotify_init();
        if (_fd_inotify < 0) {
            printf("(%.3f) Fatal error: Cannot open inotify!\n", Timer::elapsedSeconds());
            abort();
        }

        _fd_inotify_watcher = inotify_add_watch(_fd_inotify, watchedDir.c_str(), 
            (int) (IN_MOVED_TO));
        if (_fd_inotify_watcher < 0) {
            printf("(%.3f) Fatal error: Cannot open inotify watcher!\n", Timer::elapsedSeconds());
            abort();
        }

        _initialized = true;

        _bg_thread = std::thread([this]() {

            std::vector<char> _inotify_buffer;
            const size_t eventSize = sizeof(inotify_event);
            size_t bufferSize = 32'768 * eventSize + 262'144;
            _inotify_buffer.resize(bufferSize);

            while (true) {
                // Do polling
                // poll for an event to occur
                ssize_t len = read(_fd_inotify, _inotify_buffer.data(), _inotify_buffer.size());
                if (len <= 0) {
                    printf("(%.3f) Stop polling\n", Timer::elapsedSeconds());
                    break;
                }
                // digest events
                ssize_t i = 0;
                while (i < len) {
                    // digest event
                    inotify_event* event = (inotify_event*) _inotify_buffer.data()+i;
                    auto eventFile = std::string(event->name);
                    if (event->len > 0) {
                        _poll_mutex.lock();
                        _incoming_event_files.push_back(eventFile);
                        _poll_mutex.unlock();
                        _poll_cond_var.notify_all();
                    }
                    i += eventSize + event->len;
                }
            }
        });
    }
    ~EventPoller() {
        if (_initialized) {
            close(_fd_inotify_watcher);
            close(_fd_inotify);
        }
        if (_bg_thread.joinable()) _bg_thread.join();
    }

    bool initialized() const {return _initialized;}
    
    std::string poll(PollState& pollState) {
        auto lock = std::unique_lock<std::mutex>(_poll_mutex);
        if (pollState.idx >= _incoming_event_files.size()) {
            _poll_cond_var.wait(lock, [&]() {return pollState.idx < _incoming_event_files.size();});
        }
        std::string filename = _incoming_event_files[pollState.idx];
        pollState.idx++;
        return filename;
    }
};
