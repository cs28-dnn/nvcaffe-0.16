#ifndef CPU_ONLY

#include <algorithm>
#include <sstream>
#include "caffe/common.hpp"
#include "caffe/util/gpu_memory.hpp"

#include "cub/util_allocator.cuh"

namespace caffe {
using std::vector;

const int GPUMemory::INVALID_DEVICE = cub::CachingDeviceAllocator::INVALID_DEVICE_ORDINAL;
const unsigned int GPUMemory::Manager::BIN_GROWTH = 2;
const unsigned int GPUMemory::Manager::MIN_BIN = 6;
const unsigned int GPUMemory::Manager::MAX_BIN = 22;
const size_t GPUMemory::Manager::MAX_CACHED_BYTES = (size_t) -1;
const size_t GPUMemory::Manager::MAX_CACHED_SIZE = (1 << GPUMemory::Manager::MAX_BIN);  // 4M
const size_t GPUMemory::Manager::INITIAL_PINNED_BYTES = 64;
shared_mutex GPUMemory::mutex_;
mutex GPUMemory::ws_mutex_init_;

GPUMemory::Manager GPUMemory::mgr_;

vector<shared_ptr<GPUMemory::Workspace>> GPUMemory::workspace_;
vector<shared_ptr<GPUMemory::Workspace>> GPUMemory::weights_workspace_;

// To be called for every device
void GPUMemory::Init() {
  std::lock_guard<std::mutex> lock(ws_mutex_init_);
  const int device = Caffe::current_device();
  if (device + 1 > workspace_.size()) {
    workspace_.resize(device + 1);
  }
  if (!workspace_[device]) {
    workspace_[device] = make_shared<Workspace>();
  }
  if (device + 1 > weights_workspace_.size()) {
    weights_workspace_.resize(device + 1);
  }
  if (!weights_workspace_[device]) {
    weights_workspace_[device] = make_shared<Workspace>();
  }
}

void GPUMemory::Finalize() {
  std::lock_guard<std::mutex> lock(ws_mutex_init_);
  const int device = Caffe::current_device();
  if (device < workspace_.size() && workspace_[device]) {
    workspace_[device]->release();
    workspace_[device].reset();
  }
  if (device < weights_workspace_.size() && weights_workspace_[device]) {
    weights_workspace_[device]->release();
    weights_workspace_[device].reset();
  }
}

// If there is a room to grow it tries
// It keeps what it has otherwise
bool GPUMemory::Workspace::safe_reserve(size_t size, int device) {
  if (size <= size_) {
    return false;
  }
  size_t gpu_bytes_left, total_memory;
  GPUMemory::GetInfo(&gpu_bytes_left, &total_memory, true);
  if (size > size_ + align_down<7>(gpu_bytes_left)) {
    LOG(FATAL) << "Out of memory in safe_reserve: "
        << size << " > " << size_ << " + " << align_down<7>(gpu_bytes_left)
        << " on device " << device;
    return false;
  }
  release();
  reserve(size, device);  // might fail here
  return true;
}

bool GPUMemory::Workspace::try_reserve(size_t size, int device) {
  bool status = true;
  if (size > size_ || ptr_ == nullptr) {
    release();
    if (device != INVALID_DEVICE) {
      device_ = device;  // switch from default to specific one
    }
    status = mgr_.try_allocate(&ptr_, pstream_, size, device_);
    if (status) {
      CHECK_NOTNULL(ptr_);
      size_ = size;
    }
  }
  return status;
}

GPUMemory::Manager::Manager() : debug_(false), initialized_(false) {
  int count;
  CUDA_CHECK(cudaGetDeviceCount(&count));
  pinned_host_buffers_.resize(count);
  pinned_device_buffers_.resize(count);
  pinned_buffer_sizes_.resize(count);
  dev_info_.resize(count);
  update_thresholds_.resize(count);
}

bool GPUMemory::Manager::resize_buffers(int device, int group) {
  CHECK_GE(device, 0);
  CHECK_GE(group, 0);
  bool resized = false;
  if (device + 1 > pinned_buffer_sizes_.size()) {
    pinned_host_buffers_.resize(device + 1);
    pinned_device_buffers_.resize(device + 1);
    pinned_buffer_sizes_.resize(device + 1);
    resized = true;
  }
  if (group + 1 > pinned_buffer_sizes_[device].size()) {
    pinned_host_buffers_[device].resize(group + 1);
    pinned_device_buffers_[device].resize(group + 1);
    pinned_buffer_sizes_[device].resize(group + 1);
    resized = true;
  }
  return resized;
}

void* GPUMemory::thread_pinned_buffer(size_t size, int group) {
  CHECK_GT(size, 0);
  auto host_buffer_cleaner = [&](void* buffer) {
    shared_lock<shared_mutex> lock(GPUMemory::read_write_mutex());
    CUDA_CHECK(cudaFreeHost(buffer));
  };
  auto device_buffer_cleaner = [&](void* buffer) {};
  static thread_local
  unordered_map<int, unique_ptr<void, decltype(host_buffer_cleaner)>> host_buffers;
  static thread_local
  unordered_map<int, unique_ptr<void, decltype(device_buffer_cleaner)>> device_buffers;
  static thread_local vector<size_t> sizes;
  if (group + 1U > sizes.size()) {
    sizes.resize(group + 1U);
  }
  if (size > sizes[group]) {
    void* hptr;
    void* dptr;
    CUDA_CHECK(cudaHostAlloc(&hptr, size, cudaHostAllocMapped));
    CUDA_CHECK(cudaHostGetDevicePointer(&dptr, hptr, 0));
    host_buffers.emplace(std::make_pair(group,
        unique_ptr<void, decltype(host_buffer_cleaner)>(hptr, host_buffer_cleaner)));
    device_buffers.emplace(std::make_pair(group,
        unique_ptr<void, decltype(device_buffer_cleaner)>(dptr, device_buffer_cleaner)));
    sizes[group] = size;
  }
  return device_buffers.find(group)->second.get();
}

void* GPUMemory::Manager::pinned_buffer(size_t size, int device, int group) {
  const bool resized = resize_buffers(device, group);
  size = std::max(size, INITIAL_PINNED_BYTES);
  size_t current_size = pinned_buffer_sizes_[device][group];
  if (size > current_size || resized) {
    // wait for "writers" like NCCL and potentially others...
    shared_lock<shared_mutex> lock(GPUMemory::read_write_mutex());
    if (!resized) {
      cudaFreeHost(pinned_host_buffers_[device][group]);
    }
    CUDA_CHECK(cudaHostAlloc(&pinned_host_buffers_[device][group], size, cudaHostAllocMapped));
    CUDA_CHECK(cudaHostGetDevicePointer(&pinned_device_buffers_[device][group],
        pinned_host_buffers_[device][group], 0));
    pinned_buffer_sizes_[device][group] = size;
  }
  return pinned_device_buffers_[device][group];
}

void GPUMemory::Manager::init(const vector<int>& gpus, bool debug) {
  if (initialized_) {
    return;
  }
  bool debug_env = getenv("DEBUG_GPU_MEM") != 0;
  debug_ = debug || debug_env;
  try {
    // Just in case someone installed 'no cleanup' arena before
    cub_allocator_.reset(new cub::CachingDeviceAllocator(BIN_GROWTH, MIN_BIN, MAX_BIN,
        MAX_CACHED_BYTES, true, debug_));
  } catch (...) {
  }
  CHECK(cub_allocator_);
  for (int i = 0; i < gpus.size(); ++i) {
    update_dev_info(gpus[i]);
    update_thresholds_[gpus[i]] = dev_info_[gpus[i]].total_;
  }
  initialized_ = true;
  LOG(INFO) << "GPUMemory::Manager initialized";
  for (int i = 0; i < gpus.size(); ++i) {
    LOG(INFO) << report_dev_info(gpus[i]);
  }
}

void GPUMemory::Manager::reset() {
  if (!initialized_) {
    return;
  }
  cub_allocator_.reset();
  initialized_ = false;
}

GPUMemory::Manager::~Manager() {
  for (vector<void*>& buffers_group : pinned_host_buffers_) {
    for (void* buffer : buffers_group) {
      cudaFreeHost(buffer);
    }
  }
}

void GPUMemory::Manager::lazy_init(int device) {
  if (initialized_) {
    return;
  }
  if (device < 0) {
    CUDA_CHECK(cudaGetDevice(&device));
  }
  LOG(WARNING) << "Lazily initializing GPU Memory Manager Scope on device " << device
               << ". Note: it's recommended to do this explicitly in your "
                   "main() function.";
  vector<int> gpus(1, device);
  static Scope gpu_memory_scope(gpus);
}

bool GPUMemory::Manager::try_allocate(void** ptr, shared_ptr<CudaStream>& pstream,
    size_t size, int device, int group) {
  if (!initialized_) {
    lazy_init(device);
  }
  CHECK_NOTNULL(ptr);
  CHECK_EQ(current_device(), device);
  cudaError_t status = cudaSuccess, last_err = cudaSuccess;
  {
    // wait for "writers" like NCCL and potentially others
    shared_lock<shared_mutex> lock(GPUMemory::read_write_mutex());
    pstream = Caffe::thread_pstream(group);
    size_t size_allocated = 0;
    // Clean Cache & Retry logic is inside now
    status = cub_allocator_->DeviceAllocate(device, ptr, size, pstream->get(), size_allocated);
    if (status == cudaSuccess && device > INVALID_DEVICE) {
      if (size_allocated > 0) {
        if (dev_info_[device].free_ < update_thresholds_[device]) {
          update_dev_info(device);
          update_thresholds_[device] *= 0.9F;  // every 10% decrease
        } else if (dev_info_[device].free_ < size_allocated) {
          update_dev_info(device);
        } else {
          dev_info_[device].free_ -= size_allocated;
        }
      }
    }
  }
  // If there was a retry and it succeeded we get good status here but
  // we need to clean up last error...
  last_err = cudaGetLastError();
  // ...and update the dev info if something was wrong
  if (status != cudaSuccess || last_err != cudaSuccess) {
    // If we know what particular device failed we update its info only
    if (device > INVALID_DEVICE && device < dev_info_.size()) {
      // only query devices that were initialized
      if (dev_info_[device].total_) {
        update_dev_info(device);
        dev_info_[device].flush_count_++;
        DLOG(INFO) << "Updated info for device " << device << ": " << report_dev_info(device);
      }
    } else {
      // Update them all otherwise
      int cur_device;
      CUDA_CHECK(cudaGetDevice(&cur_device));
      // Refresh per-device saved values.
      for (int i = 0; i < dev_info_.size(); ++i) {
        // only query devices that were initialized
        if (dev_info_[i].total_) {
          update_dev_info(i);
          // record which device caused cache flush
          if (i == cur_device) {
            dev_info_[i].flush_count_++;
          }
          DLOG(INFO) << "Updated info for device " << i << ": " << report_dev_info(i);
        }
      }
    }
  }
  return status == cudaSuccess;
}

void GPUMemory::Manager::deallocate(void* ptr, int device) {
  // allow for null pointer deallocation
  if (!ptr || !cub_allocator_) {
    return;
  }
  int current_device;  // Just to check CUDA status:
  cudaError_t status = cudaGetDevice(&current_device);
  // Preventing dead lock while Caffe shutting down.
  if (status != cudaErrorCudartUnloading) {
    size_t size_deallocated = 0;
    // wait for "writers" like NCCL and potentially others...
    shared_lock<shared_mutex> lock(GPUMemory::read_write_mutex());
    CUDA_CHECK(cub_allocator_->DeviceFree(device, ptr, size_deallocated));
    if (size_deallocated > 0) {
      dev_info_[device].free_ += size_deallocated;
    }
  }
}

void GPUMemory::Manager::update_dev_info(int device) {
  const int initial_device = current_device();
  if (device + 1 > dev_info_.size()) {
    dev_info_.resize(device + 1);
  }
  CUDA_CHECK(cudaSetDevice(device));
  CUDA_CHECK(cudaFree(nullptr));  // initialize the context at start up
  cudaDeviceProp props;
  CUDA_CHECK(cudaGetDeviceProperties(&props, device));
  CUDA_CHECK(cudaMemGetInfo(&dev_info_[device].free_, &dev_info_[device].total_));

  // Make sure we don't have more than total device memory.
  dev_info_[device].total_ = std::min(props.totalGlobalMem, dev_info_[device].total_);
  dev_info_[device].free_ = std::min(dev_info_[device].total_, dev_info_[device].free_);
  CUDA_CHECK(cudaSetDevice(initial_device));
}

std::string GPUMemory::Manager::report_dev_info(int device) {
  cudaDeviceProp props;
  shared_lock<shared_mutex> lock(GPUMemory::read_write_mutex());
  CUDA_CHECK(cudaGetDeviceProperties(&props, device));
  DevInfo dev_info;
  CUDA_CHECK(cudaMemGetInfo(&dev_info.free_, &dev_info.total_));
  std::ostringstream os;
  os << "Total memory: " << props.totalGlobalMem << ", Free: " << dev_info.free_ << ", dev_info["
     << device << "]: total=" << dev_info_[device].total_ << " free=" << dev_info_[device].free_;
  return os.str();
}

void GPUMemory::Manager::GetInfo(size_t* free_mem, size_t* total_mem, bool with_update) {
  CHECK(cub_allocator_) << "Forgot to add 'caffe::GPUMemory::Scope gpu_memory_scope(gpus);' to your main()?";
  int cur_device;
  CUDA_CHECK(cudaGetDevice(&cur_device));
  if (with_update) {
    update_dev_info(cur_device);
  }
  *total_mem = dev_info_[cur_device].total_;
  // Free memory is free GPU memory plus free cached memory in the pool.
  *free_mem = dev_info_[cur_device].free_ + cub_allocator_->cached_bytes[cur_device].free;
  if (*free_mem > *total_mem) {  // sanity check
    *free_mem = *total_mem;
  }
}

}  // namespace caffe

#endif  // CPU_ONLY
