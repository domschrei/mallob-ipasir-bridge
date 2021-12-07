
#pragma once

#include <map>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <sys/inotify.h>
#include <iostream>
#include <unistd.h>
#include <atomic>
#include <shared_mutex>
#include <assert.h>

class EventPoller {

private:
    bool _initialized = false;
    int _fd_inotify;
    int _fd_inotify_watcher;
    std::vector<char> _inotify_buffer;

    std::mutex _poll_mutex;
    std::condition_variable _poll_cond_var;
    std::map<std::string, std::atomic_int*> _job_name_to_num_pending;

    std::atomic_bool _polling = false;

public:
    EventPoller() {}
    EventPoller(const std::string& watchedDir) {

         _fd_inotify = inotify_init();
        if (_fd_inotify < 0) {
            std::cout << "Fatal error: Cannot open inotify!" << std::endl;
            abort();
        }

        _fd_inotify_watcher = inotify_add_watch(_fd_inotify, watchedDir.c_str(), 
            (int) (IN_MOVED_TO | IN_CLOSE_WRITE));
        if (_fd_inotify_watcher < 0) {
            std::cout << "Fatal error: Cannot open inotify watcher!" << std::endl;
            abort();
        }

        size_t eventSize = sizeof(struct inotify_event);
        size_t bufferSize = 64 * eventSize + 16;
        _inotify_buffer.resize(bufferSize);

        _initialized = true;
    }

    bool initialized() const {return _initialized;}
    
    bool poll(const std::string& jobName) {

        auto lock = std::unique_lock(_poll_mutex);
        if (!_job_name_to_num_pending.count(jobName)) {
            _job_name_to_num_pending[jobName] = new std::atomic_int(0);
        }
        while (*_job_name_to_num_pending.at(jobName) <= 0) {
            bool expected = false;
            if (_polling.compare_exchange_strong(expected, true)) {
                // Do polling
                // poll for an event to occur
                lock.unlock();
                int len = read(_fd_inotify, _inotify_buffer.data(), _inotify_buffer.size());
                lock.lock();
                // digest events
                int i = 0;
                while (i < len) {
                    // digest event
                    inotify_event* event = (inotify_event*) _inotify_buffer.data()+i;
                    auto eventFile = std::string(event->name);
                    // event may be for a child solver instance
                    if (!_job_name_to_num_pending.count(eventFile)) {
                        _job_name_to_num_pending[eventFile] = new std::atomic_int(0);
                    }
                    std::cout << "digest " << eventFile << std::endl;
                    auto& numPending = *_job_name_to_num_pending.at(eventFile);
                    numPending++;
                    i += sizeof(inotify_event) + event->len;
                }
                _polling = false;
                _poll_cond_var.notify_all();
            } else {
                // Otherwise: someone else is performing polling right now
                std::cout << "wait for " << jobName << std::endl;
                _poll_cond_var.wait(lock);
            }
        }
        return true;
    }

    void unregister(const std::string& jobName) {
        _poll_mutex.lock();
        delete _job_name_to_num_pending.at(jobName); 
        _job_name_to_num_pending.erase(jobName);
        _poll_mutex.unlock();
    }

};
