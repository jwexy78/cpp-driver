/*
  Copyright (c) DataStax, Inc.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

  http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
*/

#include "event_loop.hpp"
#include "ssl.hpp"

#if !defined(_WIN32)
#include <signal.h>
#endif

namespace cass {

#if defined(HAVE_SIGTIMEDWAIT) && !defined(HAVE_NOSIGPIPE)
static int block_sigpipe() {
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGPIPE);
  return pthread_sigmask(SIG_BLOCK, &set, NULL);
}

static void consume_blocked_sigpipe() {
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGPIPE);
  struct timespec ts = { 0, 0 };
  int num = sigtimedwait(&set, NULL, &ts);
  if (num > 0) {
    LOG_WARN("Caught and ignored SIGPIPE on loop thread");
  }
}
#endif

EventLoop::EventLoop()
  : is_loop_initialized_(false)
  , is_joinable_(false)
  , is_closing_(false) { }

EventLoop::~EventLoop() {
  if (is_loop_initialized_) {
    uv_loop_close(&loop_);
  }
}

int EventLoop::init(const String& thread_name /*= ""*/) {
#if defined(_MSC_VER) && defined(_DEBUG)
  thread_name_ = thread_name;
#endif
  int rc = 0;
  rc = uv_loop_init(&loop_);
  if (rc != 0) return rc;
  rc = async_.start(loop(), this, on_task);
  if (rc != 0) return rc;
  is_loop_initialized_ = true;

#if defined(HAVE_SIGTIMEDWAIT) && !defined(HAVE_NOSIGPIPE)
  rc = block_sigpipe();
  if (rc != 0) return rc;
  rc = uv_prepare_init(loop(), &prepare_);
  if (rc != 0) return rc;
  rc = uv_prepare_start(&prepare_, on_prepare);
  if (rc != 0) return rc;
#endif
  return rc;
}

int EventLoop::run() {
  int rc = uv_thread_create(&thread_, internal_on_run, this);
  if (rc == 0) is_joinable_ = true;
  return rc;
}

void EventLoop::close_handles() {
  is_closing_.store(true);
  async_.send();
}

void EventLoop::join() {
  if (is_joinable_) {
    is_joinable_ = false;
    int rc = uv_thread_join(&thread_);
    UNUSED_(rc);
    assert(rc == 0);
  }
}

void EventLoop::add(Task* task) {
  tasks_.enqueue(task);
  async_.send();
}

void EventLoop::on_run() {
#if defined(_MSC_VER) && defined(_DEBUG)
  char temp[64];
  unsigned long thread_id = static_cast<unsigned long>(GetThreadId(uv_thread_self()));
  if (thread_name_.empty()) {
    sprintf(temp, "Event Loop - %lu", thread_id);
  } else {
    sprintf(temp, "%s - %lu", thread_name_.c_str(), thread_id);
  }
  thread_name_ = temp;
  set_thread_name(thread_name_);
#endif
}

EventLoop::TaskQueue::TaskQueue() {
  uv_mutex_init(&lock_);
}

EventLoop::TaskQueue::~TaskQueue() {
  uv_mutex_destroy(&lock_);
}

bool EventLoop::TaskQueue::enqueue(Task* task) {
  ScopedMutex l(&lock_);
  queue_.push_back(task);
  return true;
}

bool EventLoop::TaskQueue::dequeue(Task*& task) {
  ScopedMutex l(&lock_);
  if (queue_.empty()) {
    return false;
  }
  task = queue_.front();
  queue_.pop_front();
  return true;
}

bool EventLoop::TaskQueue::is_empty() {
  ScopedMutex l(&lock_);
  return queue_.empty();
}

void EventLoop::internal_on_run(void* data) {
  EventLoop* thread = static_cast<EventLoop*>(data);
  thread->handle_run();
}

void EventLoop::handle_run() {
  on_run();
  uv_run(loop(), UV_RUN_DEFAULT);
  on_after_run();
  SslContextFactory::thread_cleanup();
}

void EventLoop::on_task(Async* async) {
  EventLoop* thread = static_cast<EventLoop*>(async->data());
  thread->handle_task();
}

void EventLoop::handle_task() {
  Task* task;
  while (tasks_.dequeue(task)) {
    task->run(this);
    Memory::deallocate(task);
  }

  if (is_closing_.load() && tasks_.is_empty()) {
    async_.close_handle();
#if defined(HAVE_SIGTIMEDWAIT) && !defined(HAVE_NOSIGPIPE)
    uv_prepare_stop(&prepare_);
    uv_close(reinterpret_cast<uv_handle_t*>(&prepare_), NULL);
#endif
    is_closing_.store(false);
  }
}

#if defined(HAVE_SIGTIMEDWAIT) && !defined(HAVE_NOSIGPIPE)
void EventLoop::on_prepare(uv_prepare_t* prepare) {
  consume_blocked_sigpipe();
}
#endif

int RoundRobinEventLoopGroup::init(const String& thread_name /*= ""*/) {
  for (size_t i = 0; i < threads_.size(); ++i) {
    int rc = threads_[i].init(thread_name);
    if (rc != 0) return rc;
  }
  return 0;
}

int RoundRobinEventLoopGroup::run() {
  for (size_t i = 0; i < threads_.size(); ++i){
    int rc = threads_[i].run();
    if (rc != 0) return rc;
  }
  return 0;
}

void RoundRobinEventLoopGroup::close_handles() {
  for (size_t i = 0; i < threads_.size(); ++i) {
    threads_[i].close_handles();
  }
}

void RoundRobinEventLoopGroup::join() {
  for (size_t i = 0; i < threads_.size(); ++i) {
    threads_[i].join();
  }
}

EventLoop* RoundRobinEventLoopGroup::add(Task* task) {
  EventLoop* event_loop = &threads_[current_.fetch_add(1) % threads_.size()];
  event_loop->add(task);
  return event_loop;
}

} // namespace cass
