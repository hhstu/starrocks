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

#include "exec/spill/spilled_stream.h"

#include <glog/logging.h>

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <queue>
#include <utility>

#include "column/vectorized_fwd.h"
#include "common/status.h"
#include "exec/sort_exec_exprs.h"
#include "exec/spill/common.h"
#include "exec/spill/spiller.h"
#include "exec/spill/spiller_path_provider.h"
#include "exec/vectorized/sorting/sorting.h"
#include "fs/fs.h"
#include "runtime/runtime_state.h"
#include "runtime/sorted_chunks_merger.h"
#include "util/blocking_queue.hpp"

namespace starrocks::vectorized {
// some config
const size_t chunk_buffer_max_size = 2;

// read column one-by-one
template <class Container>
class SequentialFileStream final : public SpilledInputStream {
public:
    SequentialFileStream(const SpillFormater& formater, Container spilled_files)
            : _formater(formater), _spilled_files(std::move(spilled_files)) {}
    ~SequentialFileStream() override = default;
    StatusOr<ChunkUniquePtr> read(SpillFormatContext& context) override;
    bool is_ready() override { return false; }
    void close() override;

private:
    size_t _current_idx{};
    std::unique_ptr<RawInputStreamWrapper> _readable;
    const SpillFormater& _formater;
    const Container _spilled_files;
    size_t output_chunks{};
};

template <class Container>
StatusOr<ChunkUniquePtr> SequentialFileStream<Container>::read(SpillFormatContext& context) {
    const size_t max_eos_retry_times = 2;
    size_t eos_retry_times = 0;
    while (eos_retry_times++ < max_eos_retry_times) {
        if (_readable == nullptr) {
            if (_current_idx == _spilled_files.size()) {
                return Status::EndOfFile("eos");
            }
            ASSIGN_OR_RETURN(_readable, _spilled_files[_current_idx++]->template as<RawInputStreamWrapper>());
        }

        DCHECK(_readable != nullptr);
        auto res = _formater.restore_from_fmt(context, _readable);
        output_chunks++;
        // move cursor to next file
        if (res.status().is_end_of_file()) {
            _readable.reset();
            TRACE_SPILL_LOG << "read chunk size" << output_chunks - 1;
            output_chunks = 0;
        }

        RETURN_IF(!res.status().is_end_of_file(), res);
    }
    return nullptr;
}

template <class Container>
void SequentialFileStream<Container>::close() {
    _readable.reset();
}

class BufferedSpilledStream final : public SpilledInputStream {
public:
    BufferedSpilledStream(int capacity, std::shared_ptr<SpilledInputStream> stream)
            : _capacity(capacity), _raw_input_stream(std::move(stream)) {}
    ~BufferedSpilledStream() override = default;

    bool buffer_fulled() { return _chunk_buffer.get_size() == _capacity || eof(); }
    bool has_chunk() { return _chunk_buffer.get_size() > 0 || eof(); }

    bool is_ready() override { return has_chunk(); }
    void close() override {}

    bool acquire() {
        bool expected = false;
        return _is_running.compare_exchange_strong(expected, true);
    }

    void release() { _is_running = false; }

    StatusOr<ChunkUniquePtr> read(SpillFormatContext& context) override;

    StatusOr<ChunkUniquePtr> read_from_buffer();

    Status read_to_buffer(SpillFormatContext& context);

private:
    int _capacity;
    UnboundedBlockingQueue<ChunkUniquePtr> _chunk_buffer;
    std::shared_ptr<SpilledInputStream> _raw_input_stream;
    std::atomic_bool _is_running{};
    size_t _read_rows_from_buffer = 0;
};

StatusOr<ChunkUniquePtr> BufferedSpilledStream::read_from_buffer() {
    if (_chunk_buffer.empty()) {
        CHECK(eof());
        return Status::EndOfFile("eos");
    }
    ChunkUniquePtr res;
    CHECK(_chunk_buffer.try_get(&res));
    _read_rows_from_buffer += res->num_rows();

    return res;
}

StatusOr<ChunkUniquePtr> BufferedSpilledStream::read(SpillFormatContext& context) {
    if (has_chunk()) {
        return read_from_buffer();
    }
    return _raw_input_stream->read(context);
}

Status BufferedSpilledStream::read_to_buffer(SpillFormatContext& context) {
    DCHECK(!buffer_fulled());

    // get chunk from stream
    auto res = _raw_input_stream->read(context);
    if (res.ok()) {
        // put it to chunk buffer
        _chunk_buffer.put(std::move(res.value()));
        return Status::OK();
    } else if (res.status().is_end_of_file()) {
        mark_is_eof();
        return Status::OK();
    } else {
        return res.status();
    }
}

class SortedFileStream final : public SpilledInputStream {
public:
    SortedFileStream(SpilledInputStreamList streams, RuntimeState* state);
    ~SortedFileStream() override = default;

    Status init(const SortExecExprs* exprs, const SortDescs* descs);
    StatusOr<ChunkUniquePtr> read(SpillFormatContext& context) override;
    bool is_ready() override { return _merger.is_data_ready(); }
    void close() override {}
    std::vector<std::shared_ptr<BufferedSpilledStream>> raw_streams() { return _buffered_streams; }

private:
    std::atomic<bool> _eos{};
    SpillFormatContext _read_context;
    Status _status;

    std::vector<std::shared_ptr<BufferedSpilledStream>> _buffered_streams;

    SpilledInputStreamList _streams;
    CascadeChunkMerger _merger;
};

SortedFileStream::SortedFileStream(SpilledInputStreamList streams, RuntimeState* state)
        : _streams(std::move(streams)), _merger(state, state->runtime_profile()) {}

Status SortedFileStream::init(const SortExecExprs* sort_exprs, const SortDescs* descs) {
    std::vector<ChunkProvider> providers;
    for (auto& stream : _streams) {
        _buffered_streams.emplace_back(std::make_unique<BufferedSpilledStream>(chunk_buffer_max_size, stream));
        auto buffered_stream = _buffered_streams.back();
        auto chunk_provider = [buffered_stream, this](ChunkUniquePtr* out_chunk, bool* eos) {
            if (out_chunk == nullptr || eos == nullptr) {
                return buffered_stream->has_chunk();
            }

            if (!buffered_stream->has_chunk()) {
                return false;
            }

            auto stv = buffered_stream->read_from_buffer();
            // auto stv = buffered_stream->read(_read_context);
            if (!stv.status().ok()) {
                if (!stv.status().is_end_of_file()) {
                    _status.update(stv.status());
                }
                buffered_stream->mark_is_eof();
                *eos = true;
                return false;
            }

            *out_chunk = std::move(stv.value());
            return true;
        };
        providers.emplace_back(std::move(chunk_provider));
    }
    RETURN_IF_ERROR(_merger.init(providers, &(sort_exprs->lhs_ordering_expr_ctxs()), *descs));
    return Status::OK();
}

StatusOr<ChunkUniquePtr> SortedFileStream::read(SpillFormatContext& context) {
    ChunkUniquePtr chunk;
    bool should_exit = false;
    RETURN_IF_ERROR(_merger.get_next(&chunk, &_eos, &should_exit));
    // data is not arrived
    if (should_exit) {
        return std::make_unique<Chunk>();
    }
    if (_eos) {
        return Status::EndOfFile("eos");
    }

    return chunk;
}

class BufferedSpillReadTask final : public SpillRestoreTask {
public:
    BufferedSpillReadTask(std::weak_ptr<SpillerFactory> factory,
                          std::vector<std::shared_ptr<BufferedSpilledStream>> streams)
            : _factory(std::move(factory)), _buffered_stream(std::move(streams)) {}

    BufferedSpillReadTask(std::weak_ptr<SpillerFactory> factory, std::shared_ptr<BufferedSpilledStream> buffered_stream)
            : _factory(std::move(factory)), _buffered_stream({std::move(buffered_stream)}) {}

    Status do_read(SpillFormatContext& context) override;

private:
    std::weak_ptr<SpillerFactory> _factory;
    std::vector<std::shared_ptr<BufferedSpilledStream>> _buffered_stream;
};

Status BufferedSpillReadTask::do_read(SpillFormatContext& context) {
    size_t eofs = 0;
    for (auto& stream : _buffered_stream) {
        if (!stream->buffer_fulled()) {
            if (stream->acquire()) {
                RETURN_IF_ERROR(stream->read_to_buffer(context));
                stream->release();
            } else {
                DCHECK(false);
            }
        }
        eofs += stream->eof();
    }
    if (eofs == _buffered_stream.size()) {
        return Status::EndOfFile("eof");
    }
    return Status::OK();
}

auto SpilledFileGroup::as_flat_stream(std::weak_ptr<SpillerFactory> factory)
        -> StatusOr<std::pair<std::shared_ptr<SpilledInputStream>, std::vector<SpillRestoreTaskPtr>>> {
    // all input stream
    auto stream = std::make_shared<SequentialFileStream<decltype(_files)>>(_formater, _files);
    auto buffered_stream = std::make_shared<BufferedSpilledStream>(chunk_buffer_max_size, std::move(stream));
    auto tasks = std::vector<SpillRestoreTaskPtr>{
            std::make_shared<BufferedSpillReadTask>(std::move(factory), buffered_stream)};

    return {{buffered_stream, std::move(tasks)}};
}

auto SpilledFileGroup::as_sorted_stream(std::weak_ptr<SpillerFactory> factory, RuntimeState* state,
                                        const SortExecExprs* sort_exprs, const SortDescs* descs)
        -> StatusOr<std::pair<std::shared_ptr<SpilledInputStream>, std::vector<SpillRestoreTaskPtr>>> {
    // sorted stream
    std::vector<std::shared_ptr<SpilledInputStream>> res;
    for (auto& file : _files) {
        using ContainerType = std::array<std::shared_ptr<SpillFile>, 1>;
        auto stream = std::make_shared<SequentialFileStream<ContainerType>>(_formater, std::array{file});
        res.emplace_back(std::move(stream));
    }

    auto stream = std::make_shared<SortedFileStream>(res, state);
    RETURN_IF_ERROR(stream->init(sort_exprs, descs));
    auto tasks =
            std::vector<SpillRestoreTaskPtr>{std::make_shared<BufferedSpillReadTask>(factory, stream->raw_streams())};

    return {{stream, std::move(tasks)}};
}

} // namespace starrocks::vectorized