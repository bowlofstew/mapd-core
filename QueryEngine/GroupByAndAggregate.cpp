#include "GroupByAndAggregate.h"
#include "AggregateUtils.h"

#include "ExpressionRange.h"
#include "InPlaceSort.h"
#include "GpuInitGroups.h"

#include "Execute.h"
#include "QueryTemplateGenerator.h"
#include "ResultRows.h"
#include "RuntimeFunctions.h"
#include "../CudaMgr/CudaMgr.h"
#include "../Shared/checked_alloc.h"
#include "../Utils/ChunkIter.h"
#include "DataMgr/BufferMgr/BufferMgr.h"

#include <llvm/Transforms/Utils/BasicBlockUtils.h>

#include <numeric>
#include <thread>

QueryExecutionContext::QueryExecutionContext(const QueryMemoryDescriptor& query_mem_desc,
                                             const std::vector<int64_t>& init_agg_vals,
                                             const Executor* executor,
                                             const ExecutorDeviceType device_type,
                                             const int device_id,
                                             const std::vector<std::vector<const int8_t*>>& col_buffers,
                                             std::shared_ptr<RowSetMemoryOwner> row_set_mem_owner,
                                             const bool output_columnar,
                                             const bool sort_on_gpu,
                                             RenderAllocatorMap* render_allocator_map)
    : query_mem_desc_(query_mem_desc),
      init_agg_vals_(executor->plan_state_->init_agg_vals_),
      executor_(executor),
      device_type_(device_type),
      device_id_(device_id),
      col_buffers_(col_buffers),
      num_buffers_{device_type == ExecutorDeviceType::CPU
                       ? 1
                       : executor->blockSize() * (query_mem_desc_.blocksShareMemory() ? 1 : executor->gridSize())},
      row_set_mem_owner_(row_set_mem_owner),
      output_columnar_(output_columnar),
      sort_on_gpu_(sort_on_gpu) {
  CHECK(!sort_on_gpu_ || output_columnar);
  if (render_allocator_map || query_mem_desc_.group_col_widths.empty()) {
    allocateCountDistinctBuffers(false);
    return;
  }

  std::vector<int64_t> group_by_buffer_template(query_mem_desc_.getBufferSizeQuad(device_type));
  if (!query_mem_desc_.lazyInitGroups(device_type)) {
    if (output_columnar_) {
      initColumnarGroups(
          &group_by_buffer_template[0], &init_agg_vals[0], query_mem_desc_.entry_count, query_mem_desc_.keyless_hash);
    } else {
      initGroups(&group_by_buffer_template[0],
                 &init_agg_vals[0],
                 query_mem_desc_.entry_count,
                 query_mem_desc_.keyless_hash,
                 query_mem_desc_.interleavedBins(device_type_) ? executor_->warpSize() : 1);
    }
  }

  if (query_mem_desc_.interleavedBins(device_type_)) {
    CHECK(query_mem_desc_.keyless_hash);
  }

  if (query_mem_desc_.keyless_hash) {
    CHECK_EQ(size_t(0), query_mem_desc_.getSmallBufferSizeQuad());
  }

  std::vector<int64_t> group_by_small_buffer_template;
  if (query_mem_desc_.getSmallBufferSizeBytes()) {
    CHECK(!output_columnar_ && !query_mem_desc_.keyless_hash);
    group_by_small_buffer_template.resize(query_mem_desc_.getSmallBufferSizeQuad());
    initGroups(&group_by_small_buffer_template[0], &init_agg_vals[0], query_mem_desc_.entry_count_small, false, 1);
  }

  size_t step{device_type_ == ExecutorDeviceType::GPU && query_mem_desc_.threadsShareMemory() ? executor_->blockSize()
                                                                                              : 1};

  for (size_t i = 0; i < num_buffers_; i += step) {
    size_t index_buffer_qw{device_type_ == ExecutorDeviceType::GPU && sort_on_gpu_ && query_mem_desc_.keyless_hash
                               ? query_mem_desc_.entry_count
                               : 0};
    auto group_by_buffer = static_cast<int64_t*>(
        checked_malloc(query_mem_desc_.getBufferSizeBytes(device_type_) + index_buffer_qw * sizeof(int64_t)));
    if (!query_mem_desc_.lazyInitGroups(device_type)) {
      memcpy(group_by_buffer + index_buffer_qw,
             &group_by_buffer_template[0],
             query_mem_desc_.getBufferSizeBytes(device_type_));
    }
    row_set_mem_owner_->addGroupByBuffer(group_by_buffer);
    group_by_buffers_.push_back(group_by_buffer);
    for (size_t j = 1; j < step; ++j) {
      group_by_buffers_.push_back(nullptr);
    }
    if (query_mem_desc_.getSmallBufferSizeBytes()) {
      auto group_by_small_buffer = static_cast<int64_t*>(checked_malloc(query_mem_desc_.getSmallBufferSizeBytes()));
      row_set_mem_owner_->addGroupByBuffer(group_by_small_buffer);
      memcpy(group_by_small_buffer, &group_by_small_buffer_template[0], query_mem_desc_.getSmallBufferSizeBytes());
      small_group_by_buffers_.push_back(group_by_small_buffer);
      for (size_t j = 1; j < step; ++j) {
        small_group_by_buffers_.push_back(nullptr);
      }
    }
  }
}

void QueryExecutionContext::initColumnPerRow(int8_t* row_ptr,
                                             const size_t bin,
                                             const int64_t* init_vals,
                                             const std::vector<ssize_t>& bitmap_sizes) {
  int8_t* col_ptr = row_ptr;
  for (size_t col_idx = 0; col_idx < query_mem_desc_.agg_col_widths.size();
       col_ptr += query_mem_desc_.getNextColOffInBytes(col_ptr, bin, col_idx++)) {
    const ssize_t bm_sz{bitmap_sizes[col_idx]};
    int64_t init_val{0};
    if (!bm_sz) {
      init_val = init_vals[col_idx];
    } else {
      CHECK_EQ(static_cast<size_t>(query_mem_desc_.agg_col_widths[col_idx].compact), sizeof(int64_t));
      init_val = bm_sz > 0 ? allocateCountDistinctBitmap(bm_sz) : allocateCountDistinctSet();
    }
    switch (query_mem_desc_.agg_col_widths[col_idx].compact) {
      case 1:
        *col_ptr = static_cast<int8_t>(init_val);
        break;
      case 2:
        *reinterpret_cast<int16_t*>(col_ptr) = (int16_t)init_val;
        break;
      case 4:
        *reinterpret_cast<int32_t*>(col_ptr) = (int32_t)init_val;
        break;
      case 8:
        *reinterpret_cast<int64_t*>(col_ptr) = init_val;
        break;
      default:
        CHECK(false);
    }
  }
}

void QueryExecutionContext::initGroups(int64_t* groups_buffer,
                                       const int64_t* init_vals,
                                       const int32_t groups_buffer_entry_count,
                                       const bool keyless,
                                       const size_t warp_size) {
  const size_t key_qw_count{query_mem_desc_.group_col_widths.size()};
  const size_t row_size{query_mem_desc_.getRowSize()};
  const size_t col_base_off{query_mem_desc_.getColOffInBytes(0, 0)};

  auto agg_bitmap_size = allocateCountDistinctBuffers(true);
  auto buffer_ptr = reinterpret_cast<int8_t*>(groups_buffer);

  if (keyless) {
    CHECK(warp_size >= 1);
    CHECK(key_qw_count == 1);
    for (size_t warp_idx = 0; warp_idx < warp_size; ++warp_idx) {
      for (size_t bin = 0; bin < static_cast<size_t>(groups_buffer_entry_count); ++bin, buffer_ptr += row_size) {
        initColumnPerRow(&buffer_ptr[col_base_off], bin, init_vals, agg_bitmap_size);
      }
    }
    return;
  }

  for (size_t bin = 0; bin < static_cast<size_t>(groups_buffer_entry_count); ++bin, buffer_ptr += row_size) {
    for (size_t key_idx = 0; key_idx < key_qw_count; ++key_idx) {
      reinterpret_cast<int64_t*>(buffer_ptr)[key_idx] = EMPTY_KEY_64;
    }
    initColumnPerRow(&buffer_ptr[col_base_off], bin, init_vals, agg_bitmap_size);
  }
}

template <typename T>
int8_t* QueryExecutionContext::initColumnarBuffer(T* buffer_ptr,
                                                  const T init_val,
                                                  const uint32_t entry_count,
                                                  const ssize_t bitmap_sz,
                                                  const bool key_or_col) {
  static_assert(sizeof(T) <= sizeof(int64_t), "Unsupported template type");
  if (key_or_col) {
    for (uint32_t i = 0; i < entry_count; ++i) {
      buffer_ptr[i] = init_val;
    }
  } else {
    for (uint32_t j = 0; j < entry_count; ++j) {
      if (!bitmap_sz) {
        buffer_ptr[j] = init_val;
      } else {
        CHECK_EQ(sizeof(int64_t), sizeof(T));
        buffer_ptr[j] = bitmap_sz > 0 ? allocateCountDistinctBitmap(bitmap_sz) : allocateCountDistinctSet();
      }
    }
  }

  return reinterpret_cast<int8_t*>(buffer_ptr + entry_count);
}

void QueryExecutionContext::initColumnarGroups(int64_t* groups_buffer,
                                               const int64_t* init_vals,
                                               const int32_t groups_buffer_entry_count,
                                               const bool keyless) {
  auto agg_bitmap_size = allocateCountDistinctBuffers(true);
  const bool need_padding = !query_mem_desc_.isCompactLayoutIsometric();
  const int32_t agg_col_count = query_mem_desc_.agg_col_widths.size();
  const int32_t key_qw_count = query_mem_desc_.group_col_widths.size();
  auto buffer_ptr = reinterpret_cast<int8_t*>(groups_buffer);
  CHECK(key_qw_count == 1);
  if (!keyless) {
    buffer_ptr =
        initColumnarBuffer<int64_t>(reinterpret_cast<int64_t*>(buffer_ptr), EMPTY_KEY_64, groups_buffer_entry_count);
  }
  for (int32_t i = 0; i < agg_col_count; ++i) {
    const ssize_t bitmap_sz{agg_bitmap_size[i]};
    switch (query_mem_desc_.agg_col_widths[i].compact) {
      case 1:
        buffer_ptr = initColumnarBuffer<int8_t>(buffer_ptr, init_vals[i], bitmap_sz, false);
        break;
      case 2:
        buffer_ptr =
            initColumnarBuffer<int16_t>(reinterpret_cast<int16_t*>(buffer_ptr), init_vals[i], bitmap_sz, false);
        break;
      case 4:
        buffer_ptr =
            initColumnarBuffer<int32_t>(reinterpret_cast<int32_t*>(buffer_ptr), init_vals[i], bitmap_sz, false);
        break;
      case 8:
        buffer_ptr =
            initColumnarBuffer<int64_t>(reinterpret_cast<int64_t*>(buffer_ptr), init_vals[i], bitmap_sz, false);
        break;
      default:
        CHECK(false);
    }
    if (need_padding) {
      buffer_ptr = align_to_int64(buffer_ptr);
    }
  }
}

// deferred is true for group by queries; initGroups will allocate a bitmap
// for each group slot
std::vector<ssize_t> QueryExecutionContext::allocateCountDistinctBuffers(const bool deferred) {
  const size_t agg_col_count{query_mem_desc_.agg_col_widths.size()};
  std::vector<ssize_t> agg_bitmap_size(deferred ? agg_col_count : 0);

  CHECK_GE(agg_col_count, executor_->plan_state_->target_exprs_.size());
  for (size_t target_idx = 0, agg_col_idx = 0;
       target_idx < executor_->plan_state_->target_exprs_.size() && agg_col_idx < agg_col_count;
       ++target_idx, ++agg_col_idx) {
    const auto target_expr = executor_->plan_state_->target_exprs_[target_idx];
    const auto agg_info = target_info(target_expr);
    if (agg_info.is_distinct) {
      CHECK(agg_info.is_agg && agg_info.agg_kind == kCOUNT);
      CHECK_EQ(static_cast<size_t>(query_mem_desc_.agg_col_widths[agg_col_idx].actual), sizeof(int64_t));
      auto count_distinct_it = query_mem_desc_.count_distinct_descriptors_.find(target_idx);
      CHECK(count_distinct_it != query_mem_desc_.count_distinct_descriptors_.end());
      const auto& count_distinct_desc = count_distinct_it->second;
      if (count_distinct_desc.impl_type_ == CountDistinctImplType::Bitmap) {
        if (deferred) {
          agg_bitmap_size[agg_col_idx] = count_distinct_desc.bitmap_sz_bits;
        } else {
          init_agg_vals_[agg_col_idx] = allocateCountDistinctBitmap(count_distinct_desc.bitmap_sz_bits);
        }
      } else {
        CHECK(count_distinct_desc.impl_type_ == CountDistinctImplType::StdSet);
        if (deferred) {
          agg_bitmap_size[agg_col_idx] = -1;
        } else {
          init_agg_vals_[agg_col_idx] = allocateCountDistinctSet();
        }
      }
    }
    if (agg_info.agg_kind == kAVG) {
      ++agg_col_idx;
    }
  }

  return agg_bitmap_size;
}

int64_t QueryExecutionContext::allocateCountDistinctBitmap(const size_t bitmap_sz) {
  auto bitmap_byte_sz = bitmap_size_bytes(bitmap_sz);
  auto count_distinct_buffer = static_cast<int8_t*>(checked_calloc(bitmap_byte_sz, 1));
  row_set_mem_owner_->addCountDistinctBuffer(count_distinct_buffer);
  return reinterpret_cast<int64_t>(count_distinct_buffer);
}

int64_t QueryExecutionContext::allocateCountDistinctSet() {
  auto count_distinct_set = new std::set<int64_t>();
  row_set_mem_owner_->addCountDistinctSet(count_distinct_set);
  return reinterpret_cast<int64_t>(count_distinct_set);
}

ResultRows QueryExecutionContext::getRowSet(const std::vector<Analyzer::Expr*>& targets,
                                            const QueryMemoryDescriptor& query_mem_desc,
                                            const bool was_auto_device) const noexcept {
  std::vector<std::pair<ResultRows, std::vector<size_t>>> results_per_sm;
  CHECK_EQ(num_buffers_, group_by_buffers_.size());
  if (device_type_ == ExecutorDeviceType::CPU) {
    CHECK_EQ(size_t(1), num_buffers_);
    return groupBufferToResults(0, targets, was_auto_device);
  }
  size_t step{query_mem_desc_.threadsShareMemory() ? executor_->blockSize() : 1};
  for (size_t i = 0; i < group_by_buffers_.size(); i += step) {
    results_per_sm.emplace_back(groupBufferToResults(i, targets, was_auto_device), std::vector<size_t>{});
  }
  CHECK(device_type_ == ExecutorDeviceType::GPU);
  return executor_->reduceMultiDeviceResults(results_per_sm, row_set_mem_owner_, query_mem_desc, output_columnar_);
}

bool QueryExecutionContext::isEmptyBin(const int64_t* group_by_buffer, const size_t bin, const size_t key_idx) const {
  const size_t key_off = query_mem_desc_.getKeyOffInBytes(bin, key_idx) / sizeof(int64_t);
  if (group_by_buffer[key_off] == EMPTY_KEY_64) {
    return true;
  }
  return false;
}

#ifdef HAVE_CUDA
std::vector<CUdeviceptr> QueryExecutionContext::prepareKernelParams(
    const std::vector<std::vector<const int8_t*>>& col_buffers,
    const std::vector<int8_t>& literal_buff,
    const std::vector<int64_t>& num_rows,
    const std::vector<uint64_t>& frag_row_offsets,
    const int32_t scan_limit,
    const std::vector<int64_t>& init_agg_vals,
    const std::vector<int32_t>& error_codes,
    const uint32_t num_tables,
    const int64_t join_hash_table,
    Data_Namespace::DataMgr* data_mgr,
    const int device_id,
    const bool hoist_literals,
    const bool is_group_by) const {
  std::vector<CUdeviceptr> params(KERN_PARAM_COUNT, 0);
  uint32_t num_fragments = col_buffers.size();
  const size_t col_count{num_fragments > 0 ? col_buffers.front().size() : 0};
  if (col_count) {
    std::vector<CUdeviceptr> multifrag_col_dev_buffers;
    for (auto frag_col_buffers : col_buffers) {
      std::vector<CUdeviceptr> col_dev_buffers;
      for (auto col_buffer : frag_col_buffers) {
        col_dev_buffers.push_back(reinterpret_cast<CUdeviceptr>(col_buffer));
      }
      auto col_buffers_dev_ptr = alloc_gpu_mem(data_mgr, col_count * sizeof(CUdeviceptr), device_id, nullptr);
      copy_to_gpu(data_mgr, col_buffers_dev_ptr, &col_dev_buffers[0], col_count * sizeof(CUdeviceptr), device_id);
      multifrag_col_dev_buffers.push_back(col_buffers_dev_ptr);
    }
    params[COL_BUFFERS] = alloc_gpu_mem(data_mgr, num_fragments * sizeof(CUdeviceptr), device_id, nullptr);
    copy_to_gpu(
        data_mgr, params[COL_BUFFERS], &multifrag_col_dev_buffers[0], num_fragments * sizeof(CUdeviceptr), device_id);
  }
  params[NUM_FRAGMENTS] = alloc_gpu_mem(data_mgr, sizeof(uint32_t), device_id, nullptr);
  copy_to_gpu(data_mgr, params[NUM_FRAGMENTS], &num_fragments, sizeof(uint32_t), device_id);
  if (!literal_buff.empty()) {
    CHECK(hoist_literals);
    params[LITERALS] = alloc_gpu_mem(data_mgr, literal_buff.size(), device_id, nullptr);
    copy_to_gpu(data_mgr, params[LITERALS], &literal_buff[0], literal_buff.size(), device_id);
  }
  params[NUM_ROWS] = alloc_gpu_mem(data_mgr, sizeof(int64_t) * num_rows.size(), device_id, nullptr);
  copy_to_gpu(data_mgr, params[NUM_ROWS], &num_rows[0], sizeof(int64_t) * num_rows.size(), device_id);
  params[FRAG_ROW_OFFSETS] = alloc_gpu_mem(data_mgr, sizeof(int64_t) * frag_row_offsets.size(), device_id, nullptr);
  copy_to_gpu(
      data_mgr, params[FRAG_ROW_OFFSETS], &frag_row_offsets[0], sizeof(int64_t) * frag_row_offsets.size(), device_id);
  int32_t max_matched{scan_limit};
  params[MAX_MATCHED] = alloc_gpu_mem(data_mgr, sizeof(max_matched), device_id, nullptr);
  copy_to_gpu(data_mgr, params[MAX_MATCHED], &max_matched, sizeof(max_matched), device_id);

  int32_t total_matched{0};
  params[TOTAL_MATCHED] = alloc_gpu_mem(data_mgr, sizeof(total_matched), device_id, nullptr);
  copy_to_gpu(data_mgr, params[TOTAL_MATCHED], &total_matched, sizeof(total_matched), device_id);

  if (is_group_by && !output_columnar_) {
    auto cmpt_sz = align_to_int64(query_mem_desc_.getColsSize()) / sizeof(int64_t);
    auto cmpt_val_buff = compact_init_vals(cmpt_sz, init_agg_vals, query_mem_desc_.agg_col_widths);
    params[INIT_AGG_VALS] = alloc_gpu_mem(data_mgr, cmpt_sz * sizeof(int64_t), device_id, nullptr);
    copy_to_gpu(data_mgr, params[INIT_AGG_VALS], &cmpt_val_buff[0], cmpt_sz * sizeof(int64_t), device_id);
  } else {
    params[INIT_AGG_VALS] = alloc_gpu_mem(data_mgr, init_agg_vals.size() * sizeof(int64_t), device_id, nullptr);
    copy_to_gpu(data_mgr, params[INIT_AGG_VALS], &init_agg_vals[0], init_agg_vals.size() * sizeof(int64_t), device_id);
  }

  params[ERROR_CODE] = alloc_gpu_mem(data_mgr, error_codes.size() * sizeof(error_codes[0]), device_id, nullptr);
  copy_to_gpu(data_mgr, params[ERROR_CODE], &error_codes[0], error_codes.size() * sizeof(error_codes[0]), device_id);

  params[NUM_TABLES] = alloc_gpu_mem(data_mgr, sizeof(uint32_t), device_id, nullptr);
  copy_to_gpu(data_mgr, params[NUM_TABLES], &num_tables, sizeof(uint32_t), device_id);

  params[JOIN_HASH_TABLE] = alloc_gpu_mem(data_mgr, sizeof(int64_t), device_id, nullptr);
  copy_to_gpu(data_mgr, params[JOIN_HASH_TABLE], &join_hash_table, sizeof(int64_t), device_id);

  return params;
}

GpuQueryMemory QueryExecutionContext::prepareGroupByDevBuffer(Data_Namespace::DataMgr* data_mgr,
                                                              RenderAllocator* render_allocator,
                                                              const CUdeviceptr init_agg_vals_dev_ptr,
                                                              const int device_id,
                                                              const unsigned block_size_x,
                                                              const unsigned grid_size_x,
                                                              const bool can_sort_on_gpu) const {
  auto gpu_query_mem = create_dev_group_by_buffers(data_mgr,
                                                   group_by_buffers_,
                                                   small_group_by_buffers_,
                                                   query_mem_desc_,
                                                   block_size_x,
                                                   grid_size_x,
                                                   device_id,
                                                   can_sort_on_gpu,
                                                   false,
                                                   render_allocator);
  if (render_allocator) {
    CHECK_EQ(size_t(0), render_allocator->getAllocatedSize() % 8);
  }
  if (query_mem_desc_.lazyInitGroups(ExecutorDeviceType::GPU) &&
      query_mem_desc_.hash_type != GroupByColRangeType::MultiCol) {
    CHECK(!render_allocator);
    const size_t step{query_mem_desc_.threadsShareMemory() ? block_size_x : 1};
    size_t groups_buffer_size{query_mem_desc_.getBufferSizeBytes(ExecutorDeviceType::GPU)};
    auto group_by_dev_buffer = gpu_query_mem.group_by_buffers.second;
    const size_t col_count = query_mem_desc_.agg_col_widths.size();
    CUdeviceptr col_widths_dev_ptr{0};
    if (output_columnar_) {
      std::vector<int8_t> compact_col_widths(col_count);
      for (size_t idx = 0; idx < col_count; ++idx) {
        compact_col_widths[idx] = query_mem_desc_.agg_col_widths[idx].compact;
      }
      col_widths_dev_ptr = alloc_gpu_mem(data_mgr, col_count * sizeof(int8_t), device_id, nullptr);
      copy_to_gpu(data_mgr, col_widths_dev_ptr, &compact_col_widths[0], col_count * sizeof(int8_t), device_id);
    }
    const int8_t warp_count = query_mem_desc_.interleavedBins(ExecutorDeviceType::GPU) ? executor_->warpSize() : 1;
    for (size_t i = 0; i < group_by_buffers_.size(); i += step) {
      if (output_columnar_) {
        init_columnar_group_by_buffer_on_device(reinterpret_cast<int64_t*>(group_by_dev_buffer),
                                                reinterpret_cast<const int64_t*>(init_agg_vals_dev_ptr),
                                                query_mem_desc_.entry_count,
                                                query_mem_desc_.group_col_widths.size(),
                                                col_count,
                                                reinterpret_cast<int8_t*>(col_widths_dev_ptr),
                                                !query_mem_desc_.isCompactLayoutIsometric(),
                                                query_mem_desc_.keyless_hash,
                                                sizeof(int64_t),
                                                block_size_x,
                                                grid_size_x);
      } else {
        init_group_by_buffer_on_device(reinterpret_cast<int64_t*>(group_by_dev_buffer),
                                       reinterpret_cast<int64_t*>(init_agg_vals_dev_ptr),
                                       query_mem_desc_.entry_count,
                                       query_mem_desc_.group_col_widths.size(),
                                       query_mem_desc_.getRowSize() / sizeof(int64_t),
                                       query_mem_desc_.keyless_hash,
                                       warp_count,
                                       block_size_x,
                                       grid_size_x);
      }
      group_by_dev_buffer += groups_buffer_size;
    }
  }
  return gpu_query_mem;
}
#endif

std::vector<int64_t*> QueryExecutionContext::launchGpuCode(const std::vector<void*>& cu_functions,
                                                           const bool hoist_literals,
                                                           const std::vector<int8_t>& literal_buff,
                                                           std::vector<std::vector<const int8_t*>> col_buffers,
                                                           const std::vector<int64_t>& num_rows,
                                                           const std::vector<uint64_t>& frag_row_offsets,
                                                           const int32_t scan_limit,
                                                           const std::vector<int64_t>& init_agg_vals,
                                                           Data_Namespace::DataMgr* data_mgr,
                                                           const unsigned block_size_x,
                                                           const unsigned grid_size_x,
                                                           const int device_id,
                                                           int32_t* error_code,
                                                           const uint32_t num_tables,
                                                           const int64_t join_hash_table,
                                                           RenderAllocatorMap* render_allocator_map) const {
#ifdef HAVE_CUDA
  bool is_group_by{query_mem_desc_.getBufferSizeBytes(ExecutorDeviceType::GPU) > 0};
  data_mgr->cudaMgr_->setContext(device_id);

  RenderAllocator* render_allocator = nullptr;
  if (render_allocator_map) {
    render_allocator = render_allocator_map->getRenderAllocator(device_id);
  }

  auto cu_func = static_cast<CUfunction>(cu_functions[device_id]);
  std::vector<int64_t*> out_vec;
  uint32_t num_fragments = col_buffers.size();
  std::vector<int32_t> error_codes(grid_size_x * block_size_x);

  auto kernel_params = prepareKernelParams(col_buffers,
                                           literal_buff,
                                           num_rows,
                                           frag_row_offsets,
                                           scan_limit,
                                           init_agg_vals,
                                           error_codes,
                                           num_tables,
                                           join_hash_table,
                                           data_mgr,
                                           device_id,
                                           hoist_literals,
                                           is_group_by);

  CHECK_EQ(static_cast<size_t>(KERN_PARAM_COUNT), kernel_params.size());
  CHECK_EQ(CUdeviceptr(0), kernel_params[GROUPBY_BUF]);
  CHECK_EQ(CUdeviceptr(0), kernel_params[SMALL_BUF]);

  const unsigned block_size_y = 1;
  const unsigned block_size_z = 1;
  const unsigned grid_size_y = 1;
  const unsigned grid_size_z = 1;

  if (is_group_by) {
    CHECK(!group_by_buffers_.empty() || render_allocator);
    bool can_sort_on_gpu = query_mem_desc_.sortOnGpu();
    auto gpu_query_mem = prepareGroupByDevBuffer(data_mgr,
                                                 render_allocator,
                                                 kernel_params[INIT_AGG_VALS],
                                                 device_id,
                                                 block_size_x,
                                                 grid_size_x,
                                                 can_sort_on_gpu);

    kernel_params[GROUPBY_BUF] = gpu_query_mem.group_by_buffers.first;
    kernel_params[SMALL_BUF] = gpu_query_mem.small_group_by_buffers.first;
    std::vector<void*> param_ptrs;
    for (auto& param : kernel_params) {
      param_ptrs.push_back(&param);
    }
    if (hoist_literals) {
      checkCudaErrors(cuLaunchKernel(cu_func,
                                     grid_size_x,
                                     grid_size_y,
                                     grid_size_z,
                                     block_size_x,
                                     block_size_y,
                                     block_size_z,
                                     query_mem_desc_.sharedMemBytes(ExecutorDeviceType::GPU),
                                     nullptr,
                                     &param_ptrs[0],
                                     nullptr));
    } else {
      param_ptrs.erase(param_ptrs.begin() + LITERALS);  // TODO(alex): remove
      checkCudaErrors(cuLaunchKernel(cu_func,
                                     grid_size_x,
                                     grid_size_y,
                                     grid_size_z,
                                     block_size_x,
                                     block_size_y,
                                     block_size_z,
                                     query_mem_desc_.sharedMemBytes(ExecutorDeviceType::GPU),
                                     nullptr,
                                     &param_ptrs[0],
                                     nullptr));
    }
    if (!render_allocator) {
      copy_group_by_buffers_from_gpu(data_mgr,
                                     this,
                                     gpu_query_mem,
                                     block_size_x,
                                     grid_size_x,
                                     device_id,
                                     can_sort_on_gpu && query_mem_desc_.keyless_hash);
    }
    copy_from_gpu(
        data_mgr, &error_codes[0], kernel_params[ERROR_CODE], error_codes.size() * sizeof(error_codes[0]), device_id);
    *error_code = 0;
    for (const auto err : error_codes) {
      if (err && (!*error_code || err > *error_code)) {
        *error_code = err;
        break;
      }
    }
  } else {
    std::vector<CUdeviceptr> out_vec_dev_buffers;
    const size_t agg_col_count{init_agg_vals.size()};
    for (size_t i = 0; i < agg_col_count; ++i) {
      auto out_vec_dev_buffer =
          num_fragments
              ? alloc_gpu_mem(
                    data_mgr, block_size_x * grid_size_x * sizeof(int64_t) * num_fragments, device_id, nullptr)
              : 0;
      out_vec_dev_buffers.push_back(out_vec_dev_buffer);
    }
    auto out_vec_dev_ptr = alloc_gpu_mem(data_mgr, agg_col_count * sizeof(CUdeviceptr), device_id, nullptr);
    copy_to_gpu(data_mgr, out_vec_dev_ptr, &out_vec_dev_buffers[0], agg_col_count * sizeof(CUdeviceptr), device_id);
    CUdeviceptr unused_dev_ptr{0};
    kernel_params[GROUPBY_BUF] = out_vec_dev_ptr;
    kernel_params[SMALL_BUF] = unused_dev_ptr;
    std::vector<void*> param_ptrs;
    for (auto& param : kernel_params) {
      param_ptrs.push_back(&param);
    }
    if (hoist_literals) {
      checkCudaErrors(cuLaunchKernel(cu_func,
                                     grid_size_x,
                                     grid_size_y,
                                     grid_size_z,
                                     block_size_x,
                                     block_size_y,
                                     block_size_z,
                                     0,
                                     nullptr,
                                     &param_ptrs[0],
                                     nullptr));
    } else {
      param_ptrs.erase(param_ptrs.begin() + LITERALS);  // TODO(alex): remove
      checkCudaErrors(cuLaunchKernel(cu_func,
                                     grid_size_x,
                                     grid_size_y,
                                     grid_size_z,
                                     block_size_x,
                                     block_size_y,
                                     block_size_z,
                                     0,
                                     nullptr,
                                     &param_ptrs[0],
                                     nullptr));
    }
    for (size_t i = 0; i < agg_col_count; ++i) {
      int64_t* host_out_vec = new int64_t[block_size_x * grid_size_x * sizeof(int64_t) * num_fragments];
      copy_from_gpu(data_mgr,
                    host_out_vec,
                    out_vec_dev_buffers[i],
                    block_size_x * grid_size_x * sizeof(int64_t) * num_fragments,
                    device_id);
      out_vec.push_back(host_out_vec);
    }
  }
  return out_vec;
#else
  return {};
#endif
}

std::unique_ptr<QueryExecutionContext> QueryMemoryDescriptor::getQueryExecutionContext(
    const std::vector<int64_t>& init_agg_vals,
    const Executor* executor,
    const ExecutorDeviceType device_type,
    const int device_id,
    const std::vector<std::vector<const int8_t*>>& col_buffers,
    std::shared_ptr<RowSetMemoryOwner> row_set_mem_owner,
    const bool output_columnar,
    const bool sort_on_gpu,
    RenderAllocatorMap* render_allocator_map) const {
  return std::unique_ptr<QueryExecutionContext>(new QueryExecutionContext(*this,
                                                                          init_agg_vals,
                                                                          executor,
                                                                          device_type,
                                                                          device_id,
                                                                          col_buffers,
                                                                          row_set_mem_owner,
                                                                          output_columnar,
                                                                          sort_on_gpu,
                                                                          render_allocator_map));
}

size_t QueryMemoryDescriptor::getColsSize() const {
  CHECK(!output_columnar);
  size_t total_bytes{0};
  for (size_t col_idx = 0; col_idx < agg_col_widths.size(); ++col_idx) {
    auto chosen_bytes = agg_col_widths[col_idx].compact;
    if (chosen_bytes == sizeof(int64_t)) {
      total_bytes = align_to_int64(total_bytes);
    }
    total_bytes += chosen_bytes;
  }
  return total_bytes;
}

size_t QueryMemoryDescriptor::getRowSize() const {
  CHECK(!output_columnar);
  size_t total_bytes{0};
  if (keyless_hash) {
    CHECK_EQ(size_t(1), group_col_widths.size());
  } else {
    total_bytes += group_col_widths.size() * sizeof(int64_t);
  }
  total_bytes += getColsSize();
  return align_to_int64(total_bytes);
}

size_t QueryMemoryDescriptor::getWarpCount() const {
  return (interleaved_bins_on_gpu ? executor_->warpSize() : 1);
}

size_t QueryMemoryDescriptor::getCompactByteWidth() const {
  if (agg_col_widths.empty()) {
    return 8;
  }
  const auto compact_width = agg_col_widths.front().compact;
  for (const auto col_width : agg_col_widths) {
    CHECK_EQ(col_width.compact, compact_width);
  }
  return compact_width;
}

// TODO(miyu): remove if unnecessary
bool QueryMemoryDescriptor::isCompactLayoutIsometric() const {
  if (agg_col_widths.empty()) {
    return true;
  }
  const auto compact_width = agg_col_widths.front().compact;
  for (const auto col_width : agg_col_widths) {
    if (col_width.compact != compact_width) {
      return false;
    }
  }
  return true;
}

size_t QueryMemoryDescriptor::getTotalBytesOfColumnarBuffers(const std::vector<ColWidths>& col_widths) const {
  CHECK(output_columnar);
  size_t total_bytes{0};
  const auto is_isometric = isCompactLayoutIsometric();
  for (size_t col_idx = 0; col_idx < col_widths.size(); ++col_idx) {
    total_bytes += col_widths[col_idx].compact * entry_count;
    if (!is_isometric) {
      total_bytes = align_to_int64(total_bytes);
    }
  }
  return total_bytes;
}

size_t QueryMemoryDescriptor::getKeyOffInBytes(const size_t bin, const size_t key_idx) const {
  CHECK(!keyless_hash);
  if (output_columnar) {
    CHECK_EQ(size_t(0), key_idx);
    return bin * sizeof(int64_t);
  }

  CHECK_LT(key_idx, group_col_widths.size());
  auto offset = bin * getRowSize();
  CHECK_EQ(size_t(0), offset % sizeof(int64_t));
  offset += key_idx * sizeof(int64_t);
  return offset;
}

size_t QueryMemoryDescriptor::getNextKeyOffInBytes(const size_t crt_idx) const {
  CHECK(!keyless_hash);
  CHECK_LT(crt_idx, group_col_widths.size());
  if (output_columnar) {
    CHECK_EQ(size_t(0), crt_idx);
  }
  return sizeof(int64_t);
}

size_t QueryMemoryDescriptor::getColOnlyOffInBytes(const size_t col_idx) const {
  CHECK_LT(col_idx, agg_col_widths.size());
  size_t offset{0};
  for (size_t index = 0; index < col_idx; ++index) {
    const auto chosen_bytes = agg_col_widths[index].compact;
    if (chosen_bytes == sizeof(int64_t)) {
      offset = align_to_int64(offset);
    }
    offset += chosen_bytes;
  }

  if (sizeof(int64_t) == agg_col_widths[col_idx].compact) {
    offset = align_to_int64(offset);
  }

  return offset;
}

size_t QueryMemoryDescriptor::getColOffInBytes(const size_t bin, const size_t col_idx) const {
  CHECK_LT(col_idx, agg_col_widths.size());
  const auto warp_count = getWarpCount();
  if (output_columnar) {
    CHECK_LT(bin, entry_count);
    CHECK_EQ(size_t(1), group_col_widths.size());
    CHECK_EQ(size_t(1), warp_count);
    size_t offset{0};
    const auto is_isometric = isCompactLayoutIsometric();
    if (!keyless_hash) {
      offset = sizeof(int64_t) * entry_count;
    }
    for (size_t index = 0; index < col_idx; ++index) {
      offset += agg_col_widths[index].compact * entry_count;
      if (!is_isometric) {
        offset = align_to_int64(offset);
      }
    }
    offset += bin * agg_col_widths[col_idx].compact;
    return offset;
  }

  auto offset = bin * warp_count * getRowSize();
  if (keyless_hash) {
    CHECK_EQ(size_t(1), group_col_widths.size());
  } else {
    offset += group_col_widths.size() * sizeof(int64_t);
  }
  offset += getColOnlyOffInBytes(col_idx);
  return offset;
}

size_t QueryMemoryDescriptor::getConsistColOffInBytes(const size_t bin, const size_t col_idx) const {
  CHECK(output_columnar && !agg_col_widths.empty());
  return (keyless_hash ? 0 : sizeof(int64_t) * entry_count) + (col_idx * entry_count + bin) * agg_col_widths[0].compact;
}

size_t QueryMemoryDescriptor::getColOffInBytesInNextBin(const size_t col_idx) const {
  CHECK_LT(col_idx, agg_col_widths.size());
  auto warp_count = getWarpCount();
  if (output_columnar) {
    CHECK_EQ(size_t(1), group_col_widths.size());
    CHECK_EQ(size_t(1), warp_count);
    return agg_col_widths[col_idx].compact;
  }

  return warp_count * getRowSize();
}

size_t QueryMemoryDescriptor::getNextColOffInBytes(const int8_t* col_ptr,
                                                   const size_t bin,
                                                   const size_t col_idx) const {
  CHECK_LT(col_idx, agg_col_widths.size());
  CHECK(!output_columnar || bin < entry_count);
  size_t offset{0};
  auto warp_count = getWarpCount();
  const auto chosen_bytes = agg_col_widths[col_idx].compact;
  if (col_idx + 1 == agg_col_widths.size()) {
    if (output_columnar) {
      return (entry_count - bin) * chosen_bytes;
    } else {
      return static_cast<size_t>(align_to_int64(col_ptr + chosen_bytes) - col_ptr);
    }
  }

  const auto next_chosen_bytes = agg_col_widths[col_idx + 1].compact;
  if (output_columnar) {
    CHECK_EQ(size_t(1), group_col_widths.size());
    CHECK_EQ(size_t(1), warp_count);

    offset = entry_count * chosen_bytes;
    if (!isCompactLayoutIsometric()) {
      offset = align_to_int64(offset);
    }
    offset += bin * (next_chosen_bytes - chosen_bytes);
    return offset;
  }

  if (next_chosen_bytes == sizeof(int64_t)) {
    return static_cast<size_t>(align_to_int64(col_ptr + chosen_bytes) - col_ptr);
  } else {
    return chosen_bytes;
  }
}

size_t QueryMemoryDescriptor::getBufferSizeQuad(const ExecutorDeviceType device_type) const {
  if (keyless_hash) {
    CHECK_EQ(size_t(1), group_col_widths.size());
    auto total_bytes = align_to_int64(getColsSize());

    return (interleavedBins(device_type) ? executor_->warpSize() : 1) * entry_count * total_bytes / sizeof(int64_t);
  }

  size_t total_bytes{0};
  if (output_columnar) {
    CHECK_EQ(size_t(1), group_col_widths.size());
    total_bytes = sizeof(int64_t) * entry_count + getTotalBytesOfColumnarBuffers(agg_col_widths);
  } else {
    total_bytes = getRowSize() * entry_count;
  }

  return total_bytes / sizeof(int64_t);
}

size_t QueryMemoryDescriptor::getSmallBufferSizeQuad() const {
  CHECK(!keyless_hash || entry_count_small == 0);
  return (group_col_widths.size() + agg_col_widths.size()) * entry_count_small;
}

size_t QueryMemoryDescriptor::getBufferSizeBytes(const ExecutorDeviceType device_type) const {
  return getBufferSizeQuad(device_type) * sizeof(int64_t);
}

size_t QueryMemoryDescriptor::getSmallBufferSizeBytes() const {
  return getSmallBufferSizeQuad() * sizeof(int64_t);
}

namespace {

int32_t get_agg_count(const std::vector<Analyzer::Expr*>& target_exprs) {
  int32_t agg_count{0};
  for (auto target_expr : target_exprs) {
    CHECK(target_expr);
    const auto agg_expr = dynamic_cast<Analyzer::AggExpr*>(target_expr);
    if (!agg_expr) {
      const auto& ti = target_expr->get_type_info();
      if (ti.is_string() && ti.get_compression() != kENCODING_DICT) {
        agg_count += 2;
      } else {
        ++agg_count;
      }
      continue;
    }
    if (agg_expr && agg_expr->get_aggtype() == kAVG) {
      agg_count += 2;
    } else {
      ++agg_count;
    }
  }
  return agg_count;
}

}  // namespace

GroupByAndAggregate::ColRangeInfo GroupByAndAggregate::getColRangeInfo() {
  if (ra_exe_unit_.groupby_exprs.size() != 1) {
    try {
      checked_int64_t cardinality{1};
      bool has_nulls{false};
      for (const auto groupby_expr : ra_exe_unit_.groupby_exprs) {
        auto col_range_info = getExprRangeInfo(groupby_expr.get());
        if (col_range_info.hash_type_ != GroupByColRangeType::OneColKnownRange) {
          return {GroupByColRangeType::MultiCol, 0, 0, 0, false};
        }
        auto crt_col_cardinality = col_range_info.max - col_range_info.min + 1 + (col_range_info.has_nulls ? 1 : 0);
        CHECK_GT(crt_col_cardinality, 0);
        cardinality *= crt_col_cardinality;
        if (col_range_info.has_nulls) {
          has_nulls = true;
        }
      }
      if (cardinality > 10000000) {  // more than 10M groups is a lot
        return {GroupByColRangeType::MultiCol, 0, 0, 0, false};
      }
      return {GroupByColRangeType::MultiColPerfectHash, 0, int64_t(cardinality), 0, has_nulls};
    } catch (...) {  // overflow when computing cardinality
      return {GroupByColRangeType::MultiCol, 0, 0, 0, false};
    }
  }
  return getExprRangeInfo(ra_exe_unit_.groupby_exprs.front().get());
}

GroupByAndAggregate::ColRangeInfo GroupByAndAggregate::getExprRangeInfo(const Analyzer::Expr* expr) const {
  const int64_t guessed_range_max{255};  // TODO(alex): replace with educated guess

  const auto expr_range = getExpressionRange(expr, query_infos_, executor_);
  switch (expr_range.getType()) {
    case ExpressionRangeType::Integer:
      return {GroupByColRangeType::OneColKnownRange,
              expr_range.getIntMin(),
              expr_range.getIntMax(),
              expr_range.getBucket(),
              expr_range.hasNulls()};
    case ExpressionRangeType::FloatingPoint:
      if (g_enable_watchdog) {
        throw WatchdogException("Group by float / double would be slow");
      }
    case ExpressionRangeType::Invalid:
      return {GroupByColRangeType::OneColGuessedRange, 0, guessed_range_max, 0, false};
    default:
      CHECK(false);
  }
  CHECK(false);
  return {GroupByColRangeType::Scan, 0, 0, 0, false};
}

#define LL_CONTEXT executor_->cgen_state_->context_
#define LL_BUILDER executor_->cgen_state_->ir_builder_
#define LL_INT(v) executor_->ll_int(v)
#define ROW_FUNC executor_->cgen_state_->row_func_

namespace {

bool many_entries(const int64_t max_val, const int64_t min_val, const int64_t bucket) {
  return max_val - min_val > 10000 * std::max(bucket, int64_t(1));
}

}  // namespace

GroupByAndAggregate::GroupByAndAggregate(Executor* executor,
                                         const ExecutorDeviceType device_type,
                                         const RelAlgExecutionUnit& ra_exe_unit,
                                         const bool render_output,
                                         const std::vector<Fragmenter_Namespace::TableInfo>& query_infos,
                                         std::shared_ptr<RowSetMemoryOwner> row_set_mem_owner,
                                         const size_t max_groups_buffer_entry_count,
                                         const size_t small_groups_buffer_entry_count,
                                         const bool allow_multifrag,
                                         const bool output_columnar_hint)
    : executor_(executor), ra_exe_unit_(ra_exe_unit), query_infos_(query_infos), row_set_mem_owner_(row_set_mem_owner) {
  for (const auto groupby_expr : ra_exe_unit.groupby_exprs) {
    if (!groupby_expr) {
      continue;
    }
    const auto& groupby_ti = groupby_expr->get_type_info();
    if (groupby_ti.is_string() && groupby_ti.get_compression() != kENCODING_DICT) {
      throw std::runtime_error("Cannot group by string columns which are not dictionary encoded.");
    }
  }
  bool sort_on_gpu_hint = device_type == ExecutorDeviceType::GPU && allow_multifrag &&
                          !ra_exe_unit.order_entries.empty() && gpuCanHandleOrderEntries(ra_exe_unit.order_entries);
  initQueryMemoryDescriptor(
      allow_multifrag, max_groups_buffer_entry_count, small_groups_buffer_entry_count, sort_on_gpu_hint, render_output);
  if (device_type != ExecutorDeviceType::GPU) {
    // TODO(miyu): remove w/ interleaving
    query_mem_desc_.interleaved_bins_on_gpu = false;
  }
  query_mem_desc_.sort_on_gpu_ =
      sort_on_gpu_hint && query_mem_desc_.canOutputColumnar() && !query_mem_desc_.keyless_hash;
  query_mem_desc_.is_sort_plan = !ra_exe_unit.order_entries.empty() && !query_mem_desc_.sort_on_gpu_;
  output_columnar_ = (output_columnar_hint && query_mem_desc_.canOutputColumnar()) || query_mem_desc_.sortOnGpu();
  query_mem_desc_.output_columnar = output_columnar_;
}

namespace {

int8_t pick_target_compact_width(const RelAlgExecutionUnit& ra_exe_unit,
                                 const std::vector<Fragmenter_Namespace::TableInfo>& query_infos) {
  for (const auto groupby_expr : ra_exe_unit.groupby_exprs) {
    if (dynamic_cast<Analyzer::UOper*>(groupby_expr.get()) &&
        static_cast<Analyzer::UOper*>(groupby_expr.get())->get_optype() == kUNNEST) {
      return 8;
    }
  }
  if (ra_exe_unit.groupby_exprs.size() != 1 || !ra_exe_unit.groupby_exprs.front()) {
    return 8;
  }
  for (const auto target : ra_exe_unit.target_exprs) {
    const auto& ti = target->get_type_info();
    const auto agg = dynamic_cast<const Analyzer::AggExpr*>(target);
    if (agg && agg->get_arg()) {
      return 8;
    }
    if (agg) {
      CHECK_EQ(kCOUNT, agg->get_aggtype());
      CHECK(!agg->get_is_distinct());
      continue;
    }
    if (ti.get_type() == kINT || (ti.is_string() && ti.get_compression() == kENCODING_DICT)) {
      continue;
    } else {
      return 8;
    }
  }
  size_t total_tuples{0};
  for (const auto& query_info : query_infos) {
    total_tuples += query_info.numTuples;
  }
  return total_tuples <= static_cast<size_t>(std::numeric_limits<int32_t>::max()) ? 4 : 8;
}

}  // namespace

void GroupByAndAggregate::initQueryMemoryDescriptor(const bool allow_multifrag,
                                                    const size_t max_groups_buffer_entry_count,
                                                    const size_t small_groups_buffer_entry_count,
                                                    const bool sort_on_gpu_hint,
                                                    const bool render_output) {
  addTransientStringLiterals();

  const auto count_distinct_descriptors = initCountDistinctDescriptors();
  if (!count_distinct_descriptors.empty()) {
    CHECK(row_set_mem_owner_);
    row_set_mem_owner_->setCountDistinctDescriptors(count_distinct_descriptors);
  }

  std::vector<ColWidths> agg_col_widths;
  const auto smallest_byte_width_to_compact = pick_target_compact_width(ra_exe_unit_, query_infos_);
  for (auto wid : get_col_byte_widths(ra_exe_unit_.target_exprs)) {
    agg_col_widths.push_back({wid, static_cast<int8_t>(compact_byte_width(wid, smallest_byte_width_to_compact))});
  }
  auto group_col_widths = get_col_byte_widths(ra_exe_unit_.groupby_exprs);

  const bool is_group_by{!group_col_widths.empty()};
  if (!is_group_by) {
    CHECK(!render_output);
    query_mem_desc_ = {executor_,
                       allow_multifrag,
                       GroupByColRangeType::Scan,
                       false,
                       false,
                       -1,
                       0,
                       group_col_widths,
                       agg_col_widths,
                       0,
                       0,
                       0,
                       0,
                       0,
                       false,
                       GroupByMemSharing::Private,
                       count_distinct_descriptors,
                       false,
                       false,
                       false,
                       false};
    return;
  }

  const auto col_range_info = getColRangeInfo();

  if (g_enable_watchdog && col_range_info.hash_type_ != GroupByColRangeType::OneColKnownRange &&
      col_range_info.hash_type_ != GroupByColRangeType::MultiColPerfectHash &&
      col_range_info.hash_type_ != GroupByColRangeType::OneColGuessedRange && !render_output &&
      (ra_exe_unit_.scan_limit == 0 || ra_exe_unit_.scan_limit > 10000)) {
    throw WatchdogException("Query would use too much memory");
  }

  switch (col_range_info.hash_type_) {
    case GroupByColRangeType::OneColKnownRange:
    case GroupByColRangeType::OneColGuessedRange:
    case GroupByColRangeType::Scan: {
      if (col_range_info.hash_type_ == GroupByColRangeType::OneColGuessedRange ||
          col_range_info.hash_type_ == GroupByColRangeType::Scan ||
          ((ra_exe_unit_.groupby_exprs.size() != 1 ||
            !ra_exe_unit_.groupby_exprs.front()->get_type_info().is_string()) &&
           col_range_info.max >= col_range_info.min + static_cast<int64_t>(max_groups_buffer_entry_count) &&
           !col_range_info.bucket)) {
        const auto hash_type = render_output ? GroupByColRangeType::MultiCol : col_range_info.hash_type_;
        size_t small_group_slots =
            ra_exe_unit_.scan_limit ? static_cast<size_t>(ra_exe_unit_.scan_limit) : small_groups_buffer_entry_count;
        if (render_output) {
          small_group_slots = 0;
        }
        query_mem_desc_ = {executor_,
                           allow_multifrag,
                           hash_type,
                           false,
                           false,
                           -1,
                           0,
                           group_col_widths,
                           agg_col_widths,
                           max_groups_buffer_entry_count * (render_output ? 4 : 1),
                           small_group_slots,
                           col_range_info.min,
                           col_range_info.max,
                           0,
                           col_range_info.has_nulls,
                           GroupByMemSharing::Shared,
                           count_distinct_descriptors,
                           false,
                           false,
                           false,
                           render_output};
        return;
      } else {
        CHECK(!render_output);
        const auto keyless_info = getKeylessInfo(ra_exe_unit_.target_exprs, is_group_by);
        bool keyless =
            (!sort_on_gpu_hint || !many_entries(col_range_info.max, col_range_info.min, col_range_info.bucket)) &&
            !col_range_info.bucket && keyless_info.keyless;
        size_t bin_count = col_range_info.max - col_range_info.min;
        if (col_range_info.bucket) {
          bin_count /= col_range_info.bucket;
        }
        bin_count += (1 + (col_range_info.has_nulls ? 1 : 0));
        const size_t interleaved_max_threshold{20};
        bool interleaved_bins = keyless && (bin_count <= interleaved_max_threshold);
        query_mem_desc_ = {executor_,
                           allow_multifrag,
                           col_range_info.hash_type_,
                           keyless,
                           interleaved_bins,
                           keyless_info.target_index,
                           keyless_info.init_val,
                           group_col_widths,
                           agg_col_widths,
                           bin_count,
                           0,
                           col_range_info.min,
                           col_range_info.max,
                           col_range_info.bucket,
                           col_range_info.has_nulls,
                           GroupByMemSharing::Shared,
                           count_distinct_descriptors,
                           false,
                           false,
                           false,
                           false};
        return;
      }
    }
    case GroupByColRangeType::MultiCol: {
      CHECK(!render_output);
      query_mem_desc_ = {executor_,
                         allow_multifrag,
                         col_range_info.hash_type_,
                         false,
                         false,
                         -1,
                         0,
                         group_col_widths,
                         agg_col_widths,
                         max_groups_buffer_entry_count,
                         0,
                         0,
                         0,
                         0,
                         false,
                         GroupByMemSharing::Shared,
                         count_distinct_descriptors,
                         false,
                         false,
                         false,
                         false};
      return;
    }
    case GroupByColRangeType::MultiColPerfectHash: {
      CHECK(!render_output);
      query_mem_desc_ = {executor_,
                         allow_multifrag,
                         col_range_info.hash_type_,
                         false,
                         false,
                         -1,
                         0,
                         group_col_widths,
                         agg_col_widths,
                         static_cast<size_t>(col_range_info.max),
                         0,
                         col_range_info.min,
                         col_range_info.max,
                         0,
                         col_range_info.has_nulls,
                         GroupByMemSharing::Shared,
                         count_distinct_descriptors,
                         false,
                         false,
                         false,
                         false};
      return;
    }
    default:
      CHECK(false);
  }
  CHECK(false);
  return;
}

void GroupByAndAggregate::addTransientStringLiterals() {
  for (const auto group_expr : ra_exe_unit_.groupby_exprs) {
    if (!group_expr) {
      continue;
    }
    const auto cast_expr = dynamic_cast<const Analyzer::UOper*>(group_expr.get());
    const auto& group_ti = group_expr->get_type_info();
    if (cast_expr && cast_expr->get_optype() == kCAST && group_ti.is_string()) {
      CHECK_EQ(kENCODING_DICT, group_ti.get_compression());
      auto sd = executor_->getStringDictionary(group_ti.get_comp_param(), row_set_mem_owner_);
      CHECK(sd);
      const auto str_lit_expr = dynamic_cast<const Analyzer::Constant*>(cast_expr->get_operand());
      if (str_lit_expr && str_lit_expr->get_constval().stringval) {
        sd->getOrAddTransient(*str_lit_expr->get_constval().stringval);
      }
      continue;
    }
    const auto case_expr = dynamic_cast<const Analyzer::CaseExpr*>(group_expr.get());
    if (!case_expr) {
      continue;
    }
    Analyzer::DomainSet domain_set;
    case_expr->get_domain(domain_set);
    if (domain_set.empty()) {
      continue;
    }
    if (group_ti.is_string()) {
      CHECK_EQ(kENCODING_DICT, group_ti.get_compression());
      auto sd = executor_->getStringDictionary(group_ti.get_comp_param(), row_set_mem_owner_);
      CHECK(sd);
      for (const auto domain_expr : domain_set) {
        const auto cast_expr = dynamic_cast<const Analyzer::UOper*>(domain_expr);
        const auto str_lit_expr = cast_expr && cast_expr->get_optype() == kCAST
                                      ? dynamic_cast<const Analyzer::Constant*>(cast_expr->get_operand())
                                      : dynamic_cast<const Analyzer::Constant*>(domain_expr);
        if (str_lit_expr && str_lit_expr->get_constval().stringval) {
          sd->getOrAddTransient(*str_lit_expr->get_constval().stringval);
        }
      }
    }
  }
}

CountDistinctDescriptors GroupByAndAggregate::initCountDistinctDescriptors() {
  CountDistinctDescriptors count_distinct_descriptors;
  size_t target_idx{0};
  for (const auto target_expr : ra_exe_unit_.target_exprs) {
    auto agg_info = target_info(target_expr);
    if (agg_info.is_distinct) {
      CHECK(agg_info.is_agg);
      CHECK_EQ(kCOUNT, agg_info.agg_kind);
      const auto agg_expr = static_cast<const Analyzer::AggExpr*>(target_expr);
      const auto& arg_ti = agg_expr->get_arg()->get_type_info();
      if (arg_ti.is_string() && arg_ti.get_compression() != kENCODING_DICT) {
        throw std::runtime_error("Strings must be dictionary-encoded in COUNT(DISTINCT).");
      }
      auto arg_range_info = getExprRangeInfo(agg_expr->get_arg());
      CountDistinctImplType count_distinct_impl_type{CountDistinctImplType::StdSet};
      int64_t bitmap_sz_bits{0};
      if (arg_range_info.hash_type_ == GroupByColRangeType::OneColKnownRange &&
          !arg_ti.is_array()) {  // TODO(alex): allow bitmap implementation for arrays
        count_distinct_impl_type = CountDistinctImplType::Bitmap;
        bitmap_sz_bits = arg_range_info.max - arg_range_info.min + 1;
        const int64_t MAX_BITMAP_BITS{8 * 1000 * 1000 * 1000L};
        if (bitmap_sz_bits <= 0 || bitmap_sz_bits > MAX_BITMAP_BITS) {
          count_distinct_impl_type = CountDistinctImplType::StdSet;
        }
      }
      if (g_enable_watchdog && count_distinct_impl_type == CountDistinctImplType::StdSet) {
        throw WatchdogException("Cannot use a fast path for COUNT distinct");
      }
      CountDistinctDescriptor count_distinct_desc{
          executor_, count_distinct_impl_type, arg_range_info.min, bitmap_sz_bits};
      auto it_ok = count_distinct_descriptors.insert(std::make_pair(target_idx, count_distinct_desc));
      CHECK(it_ok.second);
    }
    ++target_idx;
  }
  return count_distinct_descriptors;
}

const QueryMemoryDescriptor& GroupByAndAggregate::getQueryMemoryDescriptor() const {
  return query_mem_desc_;
}

bool GroupByAndAggregate::outputColumnar() const {
  return output_columnar_;
}

GroupByAndAggregate::KeylessInfo GroupByAndAggregate::getKeylessInfo(
    const std::vector<Analyzer::Expr*>& target_expr_list,
    const bool is_group_by) const {
  bool keyless{true}, found{false};
  int32_t index{0};
  int64_t init_val{0};
  for (const auto target_expr : target_expr_list) {
    const auto agg_info = target_info(target_expr);
    const auto& chosen_type = get_compact_type(agg_info);
    if (!found && agg_info.is_agg) {
      auto agg_expr = dynamic_cast<const Analyzer::AggExpr*>(target_expr);
      CHECK(agg_expr);
      const auto arg_expr = agg_arg(target_expr);
      switch (agg_info.agg_kind) {
        case kAVG:
          ++index;
          init_val = 0;
          found = true;
          break;
        case kCOUNT:
          if (arg_expr && !arg_expr->get_type_info().get_notnull()) {
            auto expr_range_info = getExpressionRange(arg_expr, query_infos_, executor_);
            if (expr_range_info.hasNulls()) {
              break;
            }
          }
          init_val = 0;
          found = true;
          break;
        case kSUM: {
          if (!arg_expr->get_type_info().get_notnull()) {
            auto expr_range_info = getExpressionRange(arg_expr, query_infos_, executor_);
            if (!expr_range_info.hasNulls()) {
              init_val = get_agg_initial_val(
                  agg_info.agg_kind, arg_expr->get_type_info(), is_group_by, query_mem_desc_.getCompactByteWidth());
              found = true;
            }
          } else {
            init_val = 0;
            auto expr_range_info = getExpressionRange(arg_expr, query_infos_, executor_);
            switch (expr_range_info.getType()) {
              case ExpressionRangeType::FloatingPoint:
                if (expr_range_info.getFpMax() < 0 || expr_range_info.getFpMin() > 0) {
                  found = true;
                }
                break;
              case ExpressionRangeType::Integer:
                if (expr_range_info.getIntMax() < 0 || expr_range_info.getIntMin() > 0) {
                  found = true;
                }
                break;
              default:
                break;
            }
          }
          break;
        }
        case kMIN: {
          auto expr_range_info = getExpressionRange(agg_expr->get_arg(), query_infos_, executor_);
          auto init_max =
              get_agg_initial_val(agg_info.agg_kind, chosen_type, is_group_by, query_mem_desc_.getCompactByteWidth());
          switch (expr_range_info.getType()) {
            case ExpressionRangeType::FloatingPoint: {
              init_val = init_max;
              auto double_max = *reinterpret_cast<const double*>(&init_max);
              if (expr_range_info.getFpMax() < double_max) {
                found = true;
              }
              break;
            }
            case ExpressionRangeType::Integer:
              init_val = init_max;
              if (expr_range_info.getIntMax() < init_max) {
                found = true;
              }
              break;
            default:
              break;
          }
          break;
        }
        case kMAX: {
          auto expr_range_info = getExpressionRange(agg_expr->get_arg(), query_infos_, executor_);
          auto init_min =
              get_agg_initial_val(agg_info.agg_kind, chosen_type, is_group_by, query_mem_desc_.getCompactByteWidth());
          switch (expr_range_info.getType()) {
            case ExpressionRangeType::FloatingPoint: {
              init_val = init_min;
              auto double_min = *reinterpret_cast<const double*>(&init_min);
              if (expr_range_info.getFpMin() > double_min) {
                found = true;
              }
              break;
            }
            case ExpressionRangeType::Integer:
              init_val = init_min;
              if (expr_range_info.getIntMin() > init_min) {
                found = true;
              }
              break;
            default:
              break;
          }
          break;
        }
        default:
          keyless = false;
          break;
      }
    }
    if (!keyless) {
      break;
    }
    if (!found) {
      ++index;
    }
  }

  // shouldn't use keyless for projection only
  return {keyless && found, index, init_val};
}

bool GroupByAndAggregate::gpuCanHandleOrderEntries(const std::list<Analyzer::OrderEntry>& order_entries) {
  if (order_entries.size() > 1) {  // TODO(alex): lift this restriction
    return false;
  }
  for (const auto order_entry : order_entries) {
    CHECK_GE(order_entry.tle_no, 1);
    CHECK_LE(static_cast<size_t>(order_entry.tle_no), ra_exe_unit_.target_exprs.size());
    const auto target_expr = ra_exe_unit_.target_exprs[order_entry.tle_no - 1];
    if (!dynamic_cast<Analyzer::AggExpr*>(target_expr)) {
      return false;
    }
    // TODO(alex): relax the restrictions
    auto agg_expr = static_cast<Analyzer::AggExpr*>(target_expr);
    if (agg_expr->get_is_distinct() || agg_expr->get_aggtype() == kAVG || agg_expr->get_aggtype() == kMIN ||
        agg_expr->get_aggtype() == kMAX) {
      return false;
    }
    if (agg_expr->get_arg()) {
      auto expr_range_info = getExprRangeInfo(agg_expr->get_arg());
      if ((expr_range_info.hash_type_ != GroupByColRangeType::OneColKnownRange || expr_range_info.has_nulls) &&
          order_entry.is_desc == order_entry.nulls_first) {
        return false;
      }
    }
    const auto& target_ti = target_expr->get_type_info();
    CHECK(!target_ti.is_array());
    if (!target_ti.is_integer()) {
      return false;
    }
  }
  return true;
}

bool QueryMemoryDescriptor::usesGetGroupValueFast() const {
  return (hash_type == GroupByColRangeType::OneColKnownRange && !getSmallBufferSizeBytes());
}

bool QueryMemoryDescriptor::usesCachedContext() const {
  return allow_multifrag && (usesGetGroupValueFast() || hash_type == GroupByColRangeType::MultiColPerfectHash);
}

bool QueryMemoryDescriptor::threadsShareMemory() const {
  return sharing == GroupByMemSharing::Shared;
}

bool QueryMemoryDescriptor::blocksShareMemory() const {
  if (executor_->isCPUOnly() || render_output) {
    return true;
  }
  return usesCachedContext() && !sharedMemBytes(ExecutorDeviceType::GPU) && many_entries(max_val, min_val, bucket);
}

bool QueryMemoryDescriptor::lazyInitGroups(const ExecutorDeviceType device_type) const {
  return device_type == ExecutorDeviceType::GPU && !render_output && !getSmallBufferSizeQuad();
}

bool QueryMemoryDescriptor::interleavedBins(const ExecutorDeviceType device_type) const {
  return interleaved_bins_on_gpu && device_type == ExecutorDeviceType::GPU;
}

size_t QueryMemoryDescriptor::sharedMemBytes(const ExecutorDeviceType device_type) const {
  CHECK(device_type == ExecutorDeviceType::CPU || device_type == ExecutorDeviceType::GPU);
  if (device_type == ExecutorDeviceType::CPU) {
    return 0;
  }
  const size_t shared_mem_threshold{0};
  const size_t shared_mem_bytes{getBufferSizeBytes(ExecutorDeviceType::GPU)};
  if (!usesGetGroupValueFast() || shared_mem_bytes > shared_mem_threshold) {
    return 0;
  }
  return shared_mem_bytes;
}

bool QueryMemoryDescriptor::canOutputColumnar() const {
  return usesGetGroupValueFast() && threadsShareMemory() && blocksShareMemory() &&
         !interleavedBins(ExecutorDeviceType::GPU);
}

bool QueryMemoryDescriptor::sortOnGpu() const {
  return sort_on_gpu_;
}

GroupByAndAggregate::DiamondCodegen::DiamondCodegen(llvm::Value* cond,
                                                    Executor* executor,
                                                    const bool chain_to_next,
                                                    const std::string& label_prefix,
                                                    DiamondCodegen* parent)
    : executor_(executor), chain_to_next_(chain_to_next), parent_(parent) {
  if (parent_) {
    CHECK(!chain_to_next_);
  }
  cond_true_ = llvm::BasicBlock::Create(LL_CONTEXT, label_prefix + "_true", ROW_FUNC);
  orig_cond_false_ = cond_false_ = llvm::BasicBlock::Create(LL_CONTEXT, label_prefix + "_false", ROW_FUNC);

  LL_BUILDER.CreateCondBr(cond, cond_true_, cond_false_);
  LL_BUILDER.SetInsertPoint(cond_true_);
}

void GroupByAndAggregate::DiamondCodegen::setChainToNext() {
  CHECK(!parent_);
  chain_to_next_ = true;
}

void GroupByAndAggregate::DiamondCodegen::setFalseTarget(llvm::BasicBlock* cond_false) {
  cond_false_ = cond_false;
}

GroupByAndAggregate::DiamondCodegen::~DiamondCodegen() {
  if (parent_) {
    LL_BUILDER.CreateBr(parent_->cond_false_);
  } else if (chain_to_next_) {
    LL_BUILDER.CreateBr(cond_false_);
  }
  LL_BUILDER.SetInsertPoint(orig_cond_false_);
}

void GroupByAndAggregate::patchGroupbyCall(llvm::CallInst* call_site) {
  CHECK(call_site);
  const auto func = call_site->getCalledFunction();
  const auto func_name = func->getName();
  if (func_name == "get_columnar_group_bin_offset") {
    return;
  }

  const auto arg_count = call_site->getNumArgOperands();
  const int32_t new_size_quad = query_mem_desc_.getRowSize() / sizeof(int64_t);
  std::vector<llvm::Value*> args;
  size_t arg_idx = 0;
  auto arg_iter = func->arg_begin();
  if (func_name == "get_group_value_one_key") {
    // param 7
    for (arg_idx = 0; arg_idx < 6; ++arg_idx, ++arg_iter) {
      args.push_back(call_site->getArgOperand(arg_idx));
    }
  } else {
    // param 5
    for (arg_idx = 0; arg_idx < 4; ++arg_idx, ++arg_iter) {
      args.push_back(call_site->getArgOperand(arg_idx));
    }
  }
  CHECK(arg_iter->getName() == "row_size_quad");
  CHECK_LT(arg_idx, arg_count);
  args.push_back(LL_INT(new_size_quad));
  ++arg_idx;
  for (; arg_idx < arg_count; ++arg_idx) {
    args.push_back(call_site->getArgOperand(arg_idx));
  }
  llvm::ReplaceInstWithInst(call_site, llvm::CallInst::Create(func, args));
}

bool GroupByAndAggregate::codegen(llvm::Value* filter_result, const CompilationOptions& co) {
  CHECK(filter_result);

  bool can_return_error = false;

  {
    const bool is_group_by = !ra_exe_unit_.groupby_exprs.empty();
    const auto query_mem_desc = getQueryMemoryDescriptor();

    DiamondCodegen filter_cfg(
        filter_result, executor_, !is_group_by || query_mem_desc.usesGetGroupValueFast(), "filter");

    if (is_group_by) {
      if (ra_exe_unit_.scan_limit) {
        auto crt_match_it = ROW_FUNC->arg_begin();
        ++crt_match_it;
        ++crt_match_it;
        LL_BUILDER.CreateStore(executor_->ll_int(int32_t(1)), crt_match_it);
      }

      auto agg_out_ptr_w_idx = codegenGroupBy(co, filter_cfg);
      if (query_mem_desc.usesGetGroupValueFast() ||
          query_mem_desc.hash_type == GroupByColRangeType::MultiColPerfectHash) {
        if (query_mem_desc.hash_type == GroupByColRangeType::MultiColPerfectHash) {
          filter_cfg.setChainToNext();
        }
        // Don't generate null checks if the group slot is guaranteed to be non-null,
        // as it's the case for get_group_value_fast* family.
        codegenAggCalls(agg_out_ptr_w_idx, {}, co);
      } else {
        {
          CHECK(!outputColumnar() || query_mem_desc.keyless_hash);
          DiamondCodegen nullcheck_cfg(LL_BUILDER.CreateICmpNE(std::get<0>(agg_out_ptr_w_idx),
                                                               llvm::ConstantPointerNull::get(llvm::PointerType::get(
                                                                   get_int_type(64, LL_CONTEXT), 0))),
                                       executor_,
                                       false,
                                       "groupby_nullcheck",
                                       &filter_cfg);
          codegenAggCalls(agg_out_ptr_w_idx, {}, co);
        }
        can_return_error = true;
        LL_BUILDER.CreateRet(LL_BUILDER.CreateNeg(LL_BUILDER.CreateTrunc(
            // TODO(alex): remove the trunc once pos is converted to 32 bits
            executor_->posArg(nullptr),
            get_int_type(32, LL_CONTEXT))));
      }

      if (!outputColumnar() && query_mem_desc.getRowSize() != query_mem_desc_.getRowSize()) {
        patchGroupbyCall(static_cast<llvm::CallInst*>(std::get<0>(agg_out_ptr_w_idx)));
      }

    } else {
      auto arg_it = ROW_FUNC->arg_begin();
      std::vector<llvm::Value*> agg_out_vec;
      for (int32_t i = 0; i < get_agg_count(ra_exe_unit_.target_exprs); ++i) {
        agg_out_vec.push_back(arg_it++);
      }
      codegenAggCalls(std::make_tuple(nullptr, nullptr), agg_out_vec, co);
    }
  }

  executor_->codegenInnerScanNextRow();

  return can_return_error;
}

std::tuple<llvm::Value*, llvm::Value*> GroupByAndAggregate::codegenGroupBy(const CompilationOptions& co,
                                                                           DiamondCodegen& diamond_codegen) {
  auto arg_it = ROW_FUNC->arg_begin();
  auto groups_buffer = arg_it++;

  const int32_t row_size_quad = outputColumnar() ? 0 : query_mem_desc_.getRowSize() / sizeof(int64_t);

  std::stack<llvm::BasicBlock*> array_loops;

  switch (query_mem_desc_.hash_type) {
    case GroupByColRangeType::OneColKnownRange:
    case GroupByColRangeType::OneColGuessedRange:
    case GroupByColRangeType::Scan: {
      CHECK_EQ(size_t(1), ra_exe_unit_.groupby_exprs.size());
      const auto group_expr = ra_exe_unit_.groupby_exprs.front();
      const auto group_expr_lv = executor_->groupByColumnCodegen(
          group_expr.get(),
          co,
          query_mem_desc_.has_nulls,
          query_mem_desc_.max_val + (query_mem_desc_.bucket ? query_mem_desc_.bucket : 1),
          diamond_codegen,
          array_loops);
      auto small_groups_buffer = arg_it;
      if (query_mem_desc_.usesGetGroupValueFast()) {
        std::string get_group_fn_name{outputColumnar() && !query_mem_desc_.keyless_hash
                                          ? "get_columnar_group_bin_offset"
                                          : "get_group_value_fast"};
        if (query_mem_desc_.keyless_hash) {
          get_group_fn_name += "_keyless";
        }
        if (query_mem_desc_.interleavedBins(co.device_type_)) {
          CHECK(!outputColumnar());
          CHECK(query_mem_desc_.keyless_hash);
          get_group_fn_name += "_semiprivate";
        }
        std::vector<llvm::Value*> get_group_fn_args{
            groups_buffer, group_expr_lv, LL_INT(query_mem_desc_.min_val), LL_INT(query_mem_desc_.bucket)};
        if (!query_mem_desc_.keyless_hash) {
          if (!outputColumnar()) {
            get_group_fn_args.push_back(LL_INT(row_size_quad));
          }
        } else {
          CHECK(!outputColumnar());
          get_group_fn_args.push_back(LL_INT(row_size_quad));
          if (query_mem_desc_.interleavedBins(co.device_type_)) {
            auto warp_idx = emitCall("thread_warp_idx", {LL_INT(executor_->warpSize())});
            get_group_fn_args.push_back(warp_idx);
            get_group_fn_args.push_back(LL_INT(executor_->warpSize()));
          }
        }
        if (get_group_fn_name == "get_columnar_group_bin_offset") {
          return std::make_tuple(groups_buffer, emitCall(get_group_fn_name, get_group_fn_args));
        }
        return std::make_tuple(emitCall(get_group_fn_name, get_group_fn_args), nullptr);
      } else {
        ++arg_it;
        return std::make_tuple(emitCall("get_group_value_one_key",
                                        {groups_buffer,
                                         LL_INT(static_cast<int32_t>(query_mem_desc_.entry_count)),
                                         small_groups_buffer,
                                         LL_INT(static_cast<int32_t>(query_mem_desc_.entry_count_small)),
                                         group_expr_lv,
                                         LL_INT(query_mem_desc_.min_val),
                                         LL_INT(row_size_quad),
                                         ++arg_it}),
                               nullptr);
      }
      break;
    }
    case GroupByColRangeType::MultiCol:
    case GroupByColRangeType::MultiColPerfectHash: {
      auto key_size_lv = LL_INT(static_cast<int32_t>(query_mem_desc_.group_col_widths.size()));
      // create the key buffer
      auto group_key = LL_BUILDER.CreateAlloca(llvm::Type::getInt64Ty(LL_CONTEXT), key_size_lv);
      int32_t subkey_idx = 0;
      for (const auto group_expr : ra_exe_unit_.groupby_exprs) {
        auto col_range_info = getExprRangeInfo(group_expr.get());
        const auto group_expr_lv = executor_->groupByColumnCodegen(
            group_expr.get(), co, col_range_info.has_nulls, col_range_info.max + 1, diamond_codegen, array_loops);
        // store the sub-key to the buffer
        LL_BUILDER.CreateStore(group_expr_lv, LL_BUILDER.CreateGEP(group_key, LL_INT(subkey_idx++)));
      }
      ++arg_it;
      auto perfect_hash_func = query_mem_desc_.hash_type == GroupByColRangeType::MultiColPerfectHash
                                   ? codegenPerfectHashFunction()
                                   : nullptr;
      if (perfect_hash_func) {
        auto hash_lv = LL_BUILDER.CreateCall(perfect_hash_func, std::vector<llvm::Value*>{group_key});
        return std::make_tuple(emitCall("get_matching_group_value_perfect_hash",
                                        {groups_buffer, hash_lv, group_key, key_size_lv, LL_INT(row_size_quad)}),
                               nullptr);
      }
      return std::make_tuple(emitCall("get_group_value",
                                      {groups_buffer,
                                       LL_INT(static_cast<int32_t>(query_mem_desc_.entry_count)),
                                       group_key,
                                       key_size_lv,
                                       LL_INT(row_size_quad),
                                       ++arg_it}),
                             nullptr);
      break;
    }
    default:
      CHECK(false);
      break;
  }

  CHECK(false);
  return std::make_tuple(nullptr, nullptr);
}

llvm::Function* GroupByAndAggregate::codegenPerfectHashFunction() {
  CHECK_GT(ra_exe_unit_.groupby_exprs.size(), size_t(1));
  auto ft = llvm::FunctionType::get(get_int_type(32, LL_CONTEXT),
                                    std::vector<llvm::Type*>{llvm::PointerType::get(get_int_type(64, LL_CONTEXT), 0)},
                                    false);
  auto key_hash_func =
      llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "perfect_key_hash", executor_->cgen_state_->module_);
  executor_->cgen_state_->helper_functions_.push_back(key_hash_func);
  key_hash_func->addAttribute(llvm::AttributeSet::FunctionIndex, llvm::Attribute::AlwaysInline);
  auto& key_buff_arg = key_hash_func->getArgumentList().front();
  llvm::Value* key_buff_lv = &key_buff_arg;
  auto bb = llvm::BasicBlock::Create(LL_CONTEXT, "entry", key_hash_func);
  llvm::IRBuilder<> key_hash_func_builder(bb);
  llvm::Value* hash_lv{llvm::ConstantInt::get(get_int_type(64, LL_CONTEXT), 0)};
  std::vector<int64_t> cardinalities;
  for (const auto groupby_expr : ra_exe_unit_.groupby_exprs) {
    auto col_range_info = getExprRangeInfo(groupby_expr.get());
    CHECK(col_range_info.hash_type_ == GroupByColRangeType::OneColKnownRange);
    cardinalities.push_back(col_range_info.max - col_range_info.min + 1);
  }
  size_t dim_idx = 0;
  for (const auto groupby_expr : ra_exe_unit_.groupby_exprs) {
    auto key_comp_lv = key_hash_func_builder.CreateLoad(key_hash_func_builder.CreateGEP(key_buff_lv, LL_INT(dim_idx)));
    auto col_range_info = getExprRangeInfo(groupby_expr.get());
    auto crt_term_lv = key_hash_func_builder.CreateSub(key_comp_lv, LL_INT(col_range_info.min));
    for (size_t prev_dim_idx = 0; prev_dim_idx < dim_idx; ++prev_dim_idx) {
      crt_term_lv = key_hash_func_builder.CreateMul(crt_term_lv, LL_INT(cardinalities[prev_dim_idx]));
    }
    hash_lv = key_hash_func_builder.CreateAdd(hash_lv, crt_term_lv);
    ++dim_idx;
  }
  key_hash_func_builder.CreateRet(key_hash_func_builder.CreateTrunc(hash_lv, get_int_type(32, LL_CONTEXT)));
  return key_hash_func;
}

namespace {

std::vector<std::string> agg_fn_base_names(const TargetInfo& target_info) {
  const auto& chosen_type = get_compact_type(target_info);
  if (!target_info.is_agg) {
    if ((chosen_type.is_string() && chosen_type.get_compression() == kENCODING_NONE) || chosen_type.is_array()) {
      return {"agg_id", "agg_id"};
    }
    return {"agg_id"};
  }
  switch (target_info.agg_kind) {
    case kAVG:
      return {"agg_sum", "agg_count"};
    case kCOUNT:
      return {target_info.is_distinct ? "agg_count_distinct" : "agg_count"};
    case kMAX:
      return {"agg_max"};
    case kMIN:
      return {"agg_min"};
    case kSUM:
      return {"agg_sum"};
    default:
      CHECK(false);
  }
}

}  // namespace

llvm::Value* GroupByAndAggregate::convertNullIfAny(const SQLTypeInfo& arg_type,
                                                   const SQLTypeInfo& agg_type,
                                                   const size_t chosen_bytes,
                                                   llvm::Value* target) {
  bool need_conversion{false};
  llvm::Value* arg_null{nullptr};
  llvm::Value* agg_null{nullptr};
  llvm::Value* target_to_cast{target};
  if (arg_type.is_fp()) {
    arg_null = executor_->inlineFpNull(arg_type);
    if (agg_type.is_fp()) {
      agg_null = executor_->inlineFpNull(agg_type);
      if (!static_cast<llvm::ConstantFP*>(arg_null)
               ->isExactlyValue(static_cast<llvm::ConstantFP*>(agg_null)->getValueAPF())) {
        need_conversion = true;
      }
    } else {
      // TODO(miyu): invalid case for now
      CHECK(false);
    }
  } else {
    arg_null = executor_->inlineIntNull(arg_type);
    if (agg_type.is_fp()) {
      agg_null = executor_->inlineFpNull(agg_type);
      need_conversion = true;
      target_to_cast = executor_->castToFP(target);
    } else {
      agg_null = executor_->inlineIntNull(agg_type);
      if ((static_cast<llvm::ConstantInt*>(arg_null)->getBitWidth() !=
           static_cast<llvm::ConstantInt*>(agg_null)->getBitWidth()) ||
          (static_cast<llvm::ConstantInt*>(arg_null)->getValue() !=
           static_cast<llvm::ConstantInt*>(agg_null)->getValue())) {
        need_conversion = true;
      }
    }
  }
  if (need_conversion) {
    auto cmp =
        arg_type.is_fp() ? LL_BUILDER.CreateFCmpOEQ(target, arg_null) : LL_BUILDER.CreateICmpEQ(target, arg_null);
    return LL_BUILDER.CreateSelect(cmp, agg_null, executor_->castToTypeIn(target_to_cast, chosen_bytes << 3));
  } else {
    return target;
  }
}

void GroupByAndAggregate::codegenAggCalls(const std::tuple<llvm::Value*, llvm::Value*>& agg_out_ptr_w_idx,
                                          const std::vector<llvm::Value*>& agg_out_vec,
                                          const CompilationOptions& co) {
  // TODO(alex): unify the two cases, the output for non-group by queries
  //             should be a contiguous buffer
  const bool is_group_by{std::get<0>(agg_out_ptr_w_idx)};
  if (is_group_by) {
    CHECK(agg_out_vec.empty());
  } else {
    CHECK(!agg_out_vec.empty());
  }
  int32_t agg_out_off{0};
  for (size_t target_idx = 0; target_idx < ra_exe_unit_.target_exprs.size(); ++target_idx) {
    auto target_expr = ra_exe_unit_.target_exprs[target_idx];
    CHECK(target_expr);
    if (dynamic_cast<Analyzer::UOper*>(target_expr) &&
        static_cast<Analyzer::UOper*>(target_expr)->get_optype() == kUNNEST) {
      throw std::runtime_error("UNNEST not supported in the projection list yet.");
    }
    auto agg_info = target_info(target_expr);
    auto arg_expr = agg_arg(target_expr);
    if (arg_expr && constrained_not_null(arg_expr, ra_exe_unit_.quals)) {
      agg_info.skip_null_val = false;
    }
    const auto agg_fn_names = agg_fn_base_names(agg_info);
    auto target_lvs = codegenAggArg(target_expr, co);
    if (executor_->plan_state_->isLazyFetchColumn(target_expr) || !is_group_by) {
      // TODO(miyu): could be smaller than qword
      query_mem_desc_.agg_col_widths[agg_out_off].compact = sizeof(int64_t);
    }
    llvm::Value* str_target_lv{nullptr};
    if (target_lvs.size() == 3) {
      // none encoding string, pop the packed pointer + length since
      // it's only useful for IS NULL checks and assumed to be only
      // two components (pointer and length) for the purpose of projection
      str_target_lv = target_lvs.front();
      target_lvs.erase(target_lvs.begin());
    }
    if (target_lvs.size() < agg_fn_names.size()) {
      CHECK_EQ(size_t(1), target_lvs.size());
      CHECK_EQ(size_t(2), agg_fn_names.size());
      for (size_t i = 1; i < agg_fn_names.size(); ++i) {
        target_lvs.push_back(target_lvs.front());
      }
    } else {
      CHECK(str_target_lv || (agg_fn_names.size() == target_lvs.size()));
      CHECK(target_lvs.size() == 1 || target_lvs.size() == 2);
    }
    uint32_t col_off{0};
    const bool is_simple_count = agg_info.is_agg && agg_info.agg_kind == kCOUNT && !agg_info.is_distinct;
    if (co.device_type_ == ExecutorDeviceType::GPU && query_mem_desc_.threadsShareMemory() && is_simple_count &&
        (!arg_expr || arg_expr->get_type_info().get_notnull())) {
      CHECK_EQ(size_t(1), agg_fn_names.size());
      const auto chosen_bytes = query_mem_desc_.agg_col_widths[agg_out_off].compact;
      llvm::Value* agg_col_ptr{nullptr};
      if (is_group_by) {
        if (outputColumnar()) {
          col_off = query_mem_desc_.getColOffInBytes(0, agg_out_off);
          CHECK_EQ(size_t(0), col_off % chosen_bytes);
          col_off /= chosen_bytes;
          CHECK(std::get<1>(agg_out_ptr_w_idx));
          auto offset = LL_BUILDER.CreateAdd(std::get<1>(agg_out_ptr_w_idx), LL_INT(col_off));
          agg_col_ptr = LL_BUILDER.CreateGEP(
              LL_BUILDER.CreateBitCast(std::get<0>(agg_out_ptr_w_idx),
                                       llvm::PointerType::get(get_int_type((chosen_bytes << 3), LL_CONTEXT), 0)),
              offset);
        } else {
          col_off = query_mem_desc_.getColOnlyOffInBytes(agg_out_off);
          CHECK_EQ(size_t(0), col_off % chosen_bytes);
          col_off /= chosen_bytes;
          agg_col_ptr = LL_BUILDER.CreateGEP(
              LL_BUILDER.CreateBitCast(std::get<0>(agg_out_ptr_w_idx),
                                       llvm::PointerType::get(get_int_type((chosen_bytes << 3), LL_CONTEXT), 0)),
              LL_INT(col_off));
        }
      }

      llvm::Value* acc_i32 = nullptr;
      if (chosen_bytes != sizeof(int32_t)) {
        acc_i32 = LL_BUILDER.CreateBitCast(is_group_by ? agg_col_ptr : agg_out_vec[agg_out_off],
                                           llvm::PointerType::get(get_int_type(32, LL_CONTEXT), 0));
      } else {
        acc_i32 = (is_group_by ? agg_col_ptr : agg_out_vec[agg_out_off]);
      }
      LL_BUILDER.CreateAtomicRMW(llvm::AtomicRMWInst::Add, acc_i32, LL_INT(1), llvm::AtomicOrdering::Monotonic);
      ++agg_out_off;
      continue;
    }
    size_t target_lv_idx = 0;
    const bool lazy_fetched{executor_->plan_state_->isLazyFetchColumn(target_expr)};
    for (const auto& agg_base_name : agg_fn_names) {
      if (agg_info.is_distinct && arg_expr->get_type_info().is_array()) {
        CHECK(agg_info.is_distinct);
        CHECK_EQ(static_cast<size_t>(query_mem_desc_.agg_col_widths[agg_out_off].actual), sizeof(int64_t));
        // TODO(miyu): check if buffer may be columnar here
        CHECK(!outputColumnar());
        const auto& elem_ti = arg_expr->get_type_info().get_elem_type();
        if (is_group_by) {
          col_off = query_mem_desc_.getColOnlyOffInBytes(agg_out_off);
          CHECK_EQ(size_t(0), col_off % sizeof(int64_t));
          col_off /= sizeof(int64_t);
        }
        executor_->cgen_state_->emitExternalCall(
            "agg_count_distinct_array_" + numeric_type_name(elem_ti),
            llvm::Type::getVoidTy(LL_CONTEXT),
            {is_group_by ? LL_BUILDER.CreateGEP(std::get<0>(agg_out_ptr_w_idx), LL_INT(col_off))
                         : agg_out_vec[agg_out_off],
             target_lvs[target_lv_idx],
             executor_->posArg(arg_expr),
             elem_ti.is_fp() ? static_cast<llvm::Value*>(executor_->inlineFpNull(elem_ti))
                             : static_cast<llvm::Value*>(executor_->inlineIntNull(elem_ti))});
        ++agg_out_off;
        ++target_lv_idx;
        continue;
      }

      llvm::Value* agg_col_ptr{nullptr};
      const auto chosen_bytes = static_cast<size_t>(query_mem_desc_.agg_col_widths[agg_out_off].compact);
      const auto& chosen_type = get_compact_type(agg_info);
      if (is_group_by) {
        if (outputColumnar()) {
          col_off = query_mem_desc_.getColOffInBytes(0, agg_out_off);
          CHECK_EQ(size_t(0), col_off % chosen_bytes);
          col_off /= chosen_bytes;
          CHECK(std::get<1>(agg_out_ptr_w_idx));
          auto offset = LL_BUILDER.CreateAdd(std::get<1>(agg_out_ptr_w_idx), LL_INT(col_off));
          agg_col_ptr = LL_BUILDER.CreateGEP(
              LL_BUILDER.CreateBitCast(std::get<0>(agg_out_ptr_w_idx),
                                       llvm::PointerType::get(get_int_type((chosen_bytes << 3), LL_CONTEXT), 0)),
              offset);
        } else {
          col_off = query_mem_desc_.getColOnlyOffInBytes(agg_out_off);
          CHECK_EQ(size_t(0), col_off % chosen_bytes);
          col_off /= chosen_bytes;
          agg_col_ptr = LL_BUILDER.CreateGEP(
              LL_BUILDER.CreateBitCast(std::get<0>(agg_out_ptr_w_idx),
                                       llvm::PointerType::get(get_int_type((chosen_bytes << 3), LL_CONTEXT), 0)),
              LL_INT(col_off));
        }
      }

      auto target_lv = target_lvs[target_lv_idx];
      // TODO(miyu): check proper condition to choose skip_val version for non-groupby
      const bool need_skip_null =
          agg_info.skip_null_val && !(agg_info.agg_kind == kAVG && agg_base_name == "agg_count");
      if (need_skip_null && agg_info.agg_kind != kCOUNT) {
        target_lv = convertNullIfAny(arg_expr->get_type_info(), chosen_type, chosen_bytes, target_lv);
      } else if (!lazy_fetched && chosen_type.is_fp()) {
        target_lv = executor_->castToFP(target_lv);
      }

      if (!dynamic_cast<const Analyzer::AggExpr*>(target_expr) || arg_expr) {
        target_lv = executor_->castToTypeIn(target_lv, (chosen_bytes << 3));
      }

      std::vector<llvm::Value*> agg_args{
          is_group_by ? agg_col_ptr : executor_->castToIntPtrTyIn(agg_out_vec[agg_out_off], (chosen_bytes << 3)),
          (is_simple_count && !arg_expr) ? (chosen_bytes == sizeof(int32_t) ? LL_INT(int32_t(0)) : LL_INT(int64_t(0)))
                                         : (is_simple_count && arg_expr && str_target_lv ? str_target_lv : target_lv)};
      std::string agg_fname{agg_base_name};
      if (!lazy_fetched && chosen_type.is_fp()) {
        if (!lazy_fetched) {
          if (chosen_bytes == sizeof(float)) {
            CHECK_EQ(chosen_type.get_type(), kFLOAT);
            agg_fname += "_float";
          } else {
            CHECK_EQ(chosen_bytes, sizeof(double));
            agg_fname += "_double";
          }
        }
      } else if (chosen_bytes == sizeof(int32_t)) {
        agg_fname += "_int32";
      }

      if (agg_info.is_distinct) {
        CHECK_EQ(chosen_bytes, sizeof(int64_t));
        CHECK(!chosen_type.is_fp());
        CHECK_EQ("agg_count_distinct", agg_base_name);
        codegenCountDistinct(target_idx, target_expr, agg_args, query_mem_desc_, co.device_type_);
      } else {
        if (need_skip_null) {
          agg_fname += "_skip_val";
          auto null_lv = executor_->castToTypeIn(chosen_type.is_fp()
                                                     ? static_cast<llvm::Value*>(executor_->inlineFpNull(chosen_type))
                                                     : static_cast<llvm::Value*>(executor_->inlineIntNull(chosen_type)),
                                                 (chosen_bytes << 3));
          agg_args.push_back(null_lv);
        }
        if (!agg_info.is_distinct) {
          emitCall((co.device_type_ == ExecutorDeviceType::GPU && query_mem_desc_.threadsShareMemory())
                       ? agg_fname + "_shared"
                       : agg_fname,
                   agg_args);
        }
      }
      ++agg_out_off;
      ++target_lv_idx;
    }
  }
  for (auto target_expr : ra_exe_unit_.target_exprs) {
    CHECK(target_expr);
    executor_->plan_state_->isLazyFetchColumn(target_expr);
  }
}

void GroupByAndAggregate::codegenCountDistinct(const size_t target_idx,
                                               const Analyzer::Expr* target_expr,
                                               std::vector<llvm::Value*>& agg_args,
                                               const QueryMemoryDescriptor& query_mem_desc,
                                               const ExecutorDeviceType device_type) {
  const auto agg_info = target_info(target_expr);
  const auto& arg_ti = static_cast<const Analyzer::AggExpr*>(target_expr)->get_arg()->get_type_info();
  if (arg_ti.is_fp()) {
    agg_args.back() = executor_->cgen_state_->ir_builder_.CreateBitCast(
        agg_args.back(), get_int_type(64, executor_->cgen_state_->context_));
  }
  CHECK(device_type == ExecutorDeviceType::CPU);
  auto it_count_distinct = query_mem_desc.count_distinct_descriptors_.find(target_idx);
  CHECK(it_count_distinct != query_mem_desc.count_distinct_descriptors_.end());
  std::string agg_fname{"agg_count_distinct"};
  if (it_count_distinct->second.impl_type_ == CountDistinctImplType::Bitmap) {
    agg_fname += "_bitmap";
    agg_args.push_back(LL_INT(static_cast<int64_t>(it_count_distinct->second.min_val)));
  }
  if (agg_info.skip_null_val) {
    auto null_lv =
        executor_->castToTypeIn((arg_ti.is_fp() ? static_cast<llvm::Value*>(executor_->inlineFpNull(arg_ti))
                                                : static_cast<llvm::Value*>(executor_->inlineIntNull(arg_ti))),
                                64);
    null_lv =
        executor_->cgen_state_->ir_builder_.CreateBitCast(null_lv, get_int_type(64, executor_->cgen_state_->context_));
    agg_fname += "_skip_val";
    agg_args.push_back(null_lv);
  }
  if (it_count_distinct->second.impl_type_ == CountDistinctImplType::Bitmap) {
    emitCall(agg_fname, agg_args);
  } else {
    emitCall(agg_fname, agg_args);
  }
}

std::vector<llvm::Value*> GroupByAndAggregate::codegenAggArg(const Analyzer::Expr* target_expr,
                                                             const CompilationOptions& co) {
  const auto agg_expr = dynamic_cast<const Analyzer::AggExpr*>(target_expr);
  // TODO(alex): handle arrays uniformly?
  if (target_expr) {
    const auto& target_ti = target_expr->get_type_info();
    if (target_ti.is_array() && !executor_->plan_state_->isLazyFetchColumn(target_expr)) {
      const auto target_lvs = executor_->codegen(target_expr, !executor_->plan_state_->allow_lazy_fetch_, co);
      CHECK_EQ(size_t(1), target_lvs.size());
      CHECK(!agg_expr);
      const auto i32_ty = get_int_type(32, executor_->cgen_state_->context_);
      const auto i8p_ty = llvm::PointerType::get(get_int_type(8, executor_->cgen_state_->context_), 0);
      const auto& elem_ti = target_ti.get_elem_type();
      return {
          executor_->cgen_state_->emitExternalCall(
              "array_buff", i8p_ty, {target_lvs.front(), executor_->posArg(target_expr)}),
          executor_->cgen_state_->emitExternalCall(
              "array_size",
              i32_ty,
              {target_lvs.front(), executor_->posArg(target_expr), executor_->ll_int(log2_bytes(elem_ti.get_size()))})};
    }
  }
  return agg_expr ? executor_->codegen(agg_expr->get_arg(), true, co)
                  : executor_->codegen(target_expr, !executor_->plan_state_->allow_lazy_fetch_, co);
}

llvm::Value* GroupByAndAggregate::emitCall(const std::string& fname, const std::vector<llvm::Value*>& args) {
  return executor_->cgen_state_->emitCall(fname, args);
}

#undef ROW_FUNC
#undef LL_INT
#undef LL_BUILDER
#undef LL_CONTEXT
