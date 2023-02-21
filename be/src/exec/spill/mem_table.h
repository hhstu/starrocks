// Copyright 2021-present StarRocks, Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <functional>
#include <memory>
#include <utility>

#include "column/column_helper.h"
#include "column/vectorized_fwd.h"
#include "common/status.h"
#include "exec/vectorized/sorting/merge.h"
#include "exec/vectorized/sorting/sort_permute.h"
#include "exprs/expr_context.h"
#include "runtime/mem_tracker.h"
#include "runtime/runtime_state.h"

namespace starrocks::vectorized {
using FlushCallBack = std::function<Status(const ChunkPtr&)>;

class SpilledMemTable {
public:
    SpilledMemTable(RuntimeState* state, size_t max_buffer_size, MemTracker* parent)
            : _runtime_state(state), _max_buffer_size(max_buffer_size) {
        _tracker = std::make_unique<MemTracker>(-1, "spill-mem-table");
    }
    virtual ~SpilledMemTable() = default;
    bool is_full() { return _tracker->consumption() >= _max_buffer_size; };
    virtual Status append(ChunkPtr chunk) = 0;
    virtual Status done() = 0;
    virtual Status flush(FlushCallBack callback) = 0;

protected:
    RuntimeState* _runtime_state;
    const size_t _max_buffer_size;
    std::unique_ptr<MemTracker> _tracker;
};

using MemTablePtr = std::shared_ptr<SpilledMemTable>;

class UnorderedMemTable final : public SpilledMemTable {
public:
    template <class... Args>
    UnorderedMemTable(Args&&... args) : SpilledMemTable(std::forward<Args>(args)...) {}
    ~UnorderedMemTable() override = default;

    Status append(ChunkPtr chunk) override;
    Status done() override { return Status::OK(); };
    Status flush(FlushCallBack callback) override;

private:
    std::vector<ChunkPtr> _chunks;
};

class OrderedMemTable final : public SpilledMemTable {
public:
    template <class... Args>
    OrderedMemTable(const std::vector<ExprContext*>* sort_exprs, const SortDescs* sort_desc, Args&&... args)
            : SpilledMemTable(std::forward<Args>(args)...), _sort_exprs(sort_exprs), _sort_desc(*sort_desc) {}
    ~OrderedMemTable() override = default;

    Status append(ChunkPtr chunk) override;
    Status done() override;
    Status flush(FlushCallBack callback) override;

private:
    StatusOr<ChunkPtr> _do_sort(const ChunkPtr& chunk);

    const std::vector<ExprContext*>* _sort_exprs;
    const SortDescs _sort_desc;
    Permutation _permutation;
    ChunkPtr _chunk;
    ChunkSlice<ChunkPtr> _chunk_slice;
};

// TODO: implements ordered memtable

} // namespace starrocks::vectorized