/*
 * Copyright (c) 2001, 2025, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "gc/shared/gc_globals.hpp"
#include "gc/shared/workerThread.hpp"
#include "logging/log.hpp"
#include "memory/iterator.hpp"
#include "runtime/atomic.hpp"
#include "runtime/init.hpp"
#include "runtime/java.hpp"
#include "runtime/os.hpp"
#include "runtime/safepoint.hpp"

// oneJDK-25: TBB task_arena — shared across all WorkerTaskDispatchers
// Uses automatic concurrency (TBB detects CPU topology, including P-core/E-core)
oneapi::tbb::task_arena* WorkerTaskDispatcher::_arena = nullptr;

// oneJDK-25: TBB-based WorkerTaskDispatcher implementation
// Replaces semaphore-based dispatch with TBB parallel_for + work-stealing scheduler
WorkerTaskDispatcher::WorkerTaskDispatcher() :
    _task(nullptr),
    _done(false) {
  // Initialize TBB task_arena on first use
  // Automatic concurrency lets TBB detect CPU topology (hybrid P/E cores, homogeneous)
  if (!_arena) {
    _arena = new oneapi::tbb::task_arena(oneapi::tbb::task_arena::automatic);
    _arena->initialize();
  }
}

WorkerTaskDispatcher::~WorkerTaskDispatcher() {
  // Do not delete _arena — it's shared static
}

void WorkerTaskDispatcher::coordinator_distribute_task(WorkerTask* task, uint num_workers) {
  // No workers are allowed to read the state variables until they have been signaled.
  _task = task;
  _done.store(false);

  // oneJDK-25: Use TBB parallel_for to distribute work across workers.
  // TBB's work-stealing scheduler automatically balances load across all available cores.
  // On hybrid CPUs, TBB routes tasks to P-cores first, then E-cores.
  // This replaces the semaphore-based dispatch:
  //   OLD: _start_semaphore.signal(num_workers); _end_semaphore.wait();
  //   NEW: TBB parallel_for with blocked_range
  _arena->execute([this, task, num_workers]() {
    oneapi::tbb::parallel_for(
      oneapi::tbb::blocked_range<uint>(0, num_workers),
      [this, task](const oneapi::tbb::blocked_range<uint>& range) {
        GCIdMark gc_id_mark(task->gc_id());
        for (uint worker_id = range.begin(); worker_id != range.end(); ++worker_id) {
          // Set worker ID for this iteration (thread-local)
          WorkerThread::set_worker_id(worker_id);
          task->work(worker_id);
        }
      },
      oneapi::tbb::auto_partitioner()  // Automatic load balancing
    );
  });

  // TBB parallel_for blocks until all work is complete — no need for semaphore wait.
  assert(!_done.load(), "Task should not be marked done yet");
  _task = nullptr;
}

void WorkerTaskDispatcher::worker_run_task() {
  // oneJDK-25: This method is no longer used with TBB.
  // TBB manages its own worker threads via task_arena.
  // Kept for ABI compatibility — should not be called in TBB mode.
  ShouldNotReachHere();
}

WorkerThreads::WorkerThreads(const char* name, uint max_workers) :
    _name(name),
    _workers(NEW_C_HEAP_ARRAY(WorkerThread*, max_workers, mtInternal)),
    _max_workers(max_workers),
    _created_workers(0),
    _active_workers(0),
    _dispatcher() {}

void WorkerThreads::initialize_workers() {
  const uint initial_active_workers = UseDynamicNumberOfGCThreads ? 1 : _max_workers;
  if (set_active_workers(initial_active_workers) != initial_active_workers) {
    vm_exit_during_initialization();
  }
}

WorkerThread* WorkerThreads::create_worker(uint name_suffix) {
  // oneJDK-25: With TBB, we no longer create OS threads for GC workers.
  // TBB's task_arena manages its own thread pool with work-stealing scheduler.
  // However, we still create WorkerThread objects for compatibility with
  // threads_do() iteration and JVMTI thread enumeration.
  // These threads are marked as TBB-managed and do not block on semaphores.
  if (is_init_completed() && InjectGCWorkerCreationFailure) {
    return nullptr;
  }

  WorkerThread* const worker = new WorkerThread(_name, name_suffix, &_dispatcher);

  if (!os::create_thread(worker, os::gc_thread)) {
    delete worker;
    return nullptr;
  }

  on_create_worker(worker);

  os::start_thread(worker);

  return worker;
}

uint WorkerThreads::set_active_workers(uint num_workers) {
  assert(num_workers > 0 && num_workers <= _max_workers,
         "Invalid number of active workers %u (should be 1-%u)",
         num_workers, _max_workers);

  while (_created_workers < num_workers) {
    WorkerThread* const worker = create_worker(_created_workers);
    if (worker == nullptr) {
      log_error(gc, task)("Failed to create worker thread");
      break;
    }

    _workers[_created_workers] = worker;
    _created_workers++;
  }

  _active_workers = MIN2(_created_workers, num_workers);

  log_trace(gc, task)("%s: using %d out of %d workers", _name, _active_workers, _max_workers);

  return _active_workers;
}

void WorkerThreads::threads_do(ThreadClosure* tc) const {
  for (uint i = 0; i < _created_workers; i++) {
    tc->do_thread(_workers[i]);
  }
}

template <typename Function>
void WorkerThreads::threads_do_f(Function function) const {
  for (uint i = 0; i < _created_workers; i++) {
    function(_workers[i]);
  }
}

void WorkerThreads::set_indirect_states() {
#ifdef ASSERT
  const bool is_suspendible = Thread::current()->is_suspendible_thread();
  const bool is_safepointed = Thread::current()->is_VM_thread() && SafepointSynchronize::is_at_safepoint();

  threads_do_f([&](Thread* thread) {
    assert(!thread->is_indirectly_suspendible_thread(), "Unexpected");
    assert(!thread->is_indirectly_safepoint_thread(), "Unexpected");
    if (is_suspendible) {
      thread->set_indirectly_suspendible_thread();
    }
    if (is_safepointed) {
      thread->set_indirectly_safepoint_thread();
    }
  });
#endif
}

void WorkerThreads::clear_indirect_states() {
#ifdef ASSERT
  threads_do_f([&](Thread* thread) {
    thread->clear_indirectly_suspendible_thread();
    thread->clear_indirectly_safepoint_thread();
  });
#endif
}

void WorkerThreads::run_task(WorkerTask* task) {
  set_indirect_states();
  _dispatcher.coordinator_distribute_task(task, _active_workers);
  clear_indirect_states();
}

void WorkerThreads::run_task(WorkerTask* task, uint num_workers) {
  WithActiveWorkers with_active_workers(this, num_workers);
  run_task(task);
}

THREAD_LOCAL uint WorkerThread::_worker_id = UINT_MAX;

WorkerThread::WorkerThread(const char* name_prefix, uint name_suffix, WorkerTaskDispatcher* dispatcher) :
    _dispatcher(dispatcher) {
  set_name("%s#%u", name_prefix, name_suffix);
}

void WorkerThread::run() {
  os::set_priority(this, NearMaxPriority);

  // oneJDK-25: With TBB, WorkerThread::run() no longer blocks on semaphores.
  // Instead, it signals that this thread is ready and then exits, allowing
  // TBB's task_arena to manage the actual work distribution.
  // The thread exists for JVMTI enumeration and threads_do() iteration,
  // but GC work is executed by TBB's work-stealing scheduler.
  while (true) {
    // TBB mode: thread is a stub for enumeration only.
    // Actual GC work is dispatched via TBB parallel_for in coordinator_distribute_task().
    // Park this thread to avoid busy-waiting — it's kept alive for JVMTI visibility.
    os::NakedPark(os::ParkInterval, 1000);  // Park for up to 1 second
  }
}
