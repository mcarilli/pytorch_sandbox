#include "torch/csrc/autograd/profiler.h"
#include "torch/csrc/autograd/function.h"
#include <sstream>

namespace torch { namespace autograd { namespace profiler {

ProfilerState state = ProfilerState::Disabled;
uint32_t next_thread_id = 0;
std::mutex all_event_lists_mutex;
std::list<std::shared_ptr<RangeEventList>> all_event_lists;
thread_local std::shared_ptr<RangeEventList> event_list;
thread_local int32_t thread_id;

RangeEventList& getEventList() {
  if (!event_list) {
    std::lock_guard<std::mutex> guard(all_event_lists_mutex);
    event_list = std::make_shared<RangeEventList>();
    thread_id = next_thread_id++;
    all_event_lists.emplace_front(event_list);
  }
  return *event_list;
}

void mark(std::string name, bool include_cuda /* = true */) {
  if (state == ProfilerState::NVTX) {
#ifdef USE_CUDA
    nvtxMarkA(name.c_str());
#else
    throw std::logic_error(
        "mark called with NVTX tracing, but compiled without CUDA");
#endif
  } else {
    getEventList().record(
        EventKind::Mark,
        std::move(name),
        thread_id,
        include_cuda && state == ProfilerState::CUDA);
  }
}

void pushRange(std::string name) {
  if (state == ProfilerState::Disabled) {
    return;
  }
  if (state == ProfilerState::NVTX) {
#ifdef USE_CUDA
    nvtxRangePushA(name.c_str());
#else
    throw std::logic_error(
        "pushRange called with NVTX tracing, but compiled without CUDA");
#endif
  } else {
    getEventList().record(
        EventKind::PushRange,
        std::move(name),
        thread_id,
        state == ProfilerState::CUDA);
  }
}

void popRange() {
  if (state == ProfilerState::Disabled) {
    return;
  }
  if (state == ProfilerState::NVTX) {
#ifdef USE_CUDA
    nvtxRangePop();
#else
    throw std::logic_error(
        "popRange called with NVTX tracing, but compiled without CUDA");
#endif
  } else {
    getEventList().record(
        EventKind::PopRange,
        std::string(),
        thread_id,
        state == ProfilerState::CUDA);
  }
}

RecordFunction::RecordFunction(Function* fn) {
  if (state == ProfilerState::Disabled)
    return;
  pushFunctionRange(fn);
}

RecordFunction::RecordFunction(std::string name) {
  if (state == ProfilerState::Disabled)
    return;
  pushRange(std::move(name));
}

RecordFunction::RecordFunction(const char* name) {
  if (state == ProfilerState::Disabled)
    return;
  pushRange(name);
}

RecordFunction::RecordFunction(const char* name, int64_t current_sequence_nr) 
{
  if (state == ProfilerState::Disabled)
    return;
  std::stringstream s;
  s << name << ", current seq nr " << current_sequence_nr;
  if(backward_apply_state)
    s << ", backward apply seq nr " << backward_apply_sequence_nr;
  pushRange(std::move(s.str()));
}

RecordFunction::~RecordFunction() {
  if (state == ProfilerState::Disabled)
    return;
  popRange();
}

thread_local bool RecordFunction::backward_apply_state;
thread_local int64_t RecordFunction::backward_apply_sequence_nr;

void RecordFunction::set_backward_apply_state(bool state, int64_t backward_apply_nr)
{
  backward_apply_state = state;
  backward_apply_sequence_nr = backward_apply_nr;
}

void RecordFunction::pushFunctionRange(Function* fn) {
  pushRange(fn->name());
}

#ifdef USE_CUDA
static void onEachDevice(std::function<void(int)> op) {
  at::DeviceGuard device_guard;
  int count;
  TORCH_CUDA_CHECK(cudaGetDeviceCount(&count));
  for(int i = 0; i < count; i++) {
    device_guard.set_index(i);
    op(i);
  }
}
#endif

void enableProfiler(ProfilerState new_state) {
  AT_ASSERT(new_state != ProfilerState::Disabled);
#ifndef USE_CUDA
  if (new_state == ProfilerState::NVTX)
    throw std::runtime_error("Can't use NVTX profiler - PyTorch was compiled without CUDA");
#endif
  if (state != ProfilerState::Disabled && new_state != state) {
      throw std::runtime_error("can't change kind of profiling (e.g. NVTX to CPU) while profiler is running");
  }
  state = new_state;

#ifdef USE_CUDA
  if(state == ProfilerState::CUDA) {
    // event recording appears to have some startup overhead, so we need to
    // to generate some dummy events first before recording syncrhonization events
    for(int i = 0; i < 5; i++) {
      onEachDevice([](int d) {
          mark("__cuda_startup");
          cudaDeviceSynchronize();
      });
    }

    // cuda events must be on the same device, so we need a start event recorded
    // for each gpu. we then use this event to synchronize time on the GPU
    // with the CPU clock.
    onEachDevice([](int d) {
        mark("__cuda_start_event");
    });
  }
#endif
  mark("__start_profile", false);
}

thread_event_lists disableProfiler() {
  if (state == ProfilerState::Disabled) {
    throw std::runtime_error("can't disable profiler when it's not running");
  }
  ProfilerState old_state = state;
  mark("__stop_profile");
  state = ProfilerState::Disabled;
  if (old_state == ProfilerState::NVTX) {
    return thread_event_lists();
  } else {
    thread_event_lists result;
    std::lock_guard<std::mutex> guard(all_event_lists_mutex);
    for (auto it = all_event_lists.begin(); it != all_event_lists.end();) {
      auto & list = *it;
      result.emplace_back(list->consolidate());
      // GC lists that are not held by any threads
      if (list.use_count() == 1) {
        auto current_it = it;
        ++it;
        all_event_lists.erase(current_it);
      } else {
        ++it;
      }
    }
    return result;
  }
}

}}}
