/*
  Copyright (c) 2014-2015 DataStax

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

#include "query_request.hpp"

#include "logger.hpp"
#include "serialization.hpp"

namespace cass {

int32_t QueryRequest::encode_batch(int version, BufferVec* bufs) const {
  int32_t length = 0;
  const std::string& query(query_);

  // <kind><string><n>[name_1]<value_1>...[name_n]<value_n> ([byte][long string][short][bytes]...[bytes])
  int buf_size = sizeof(uint8_t) + sizeof(uint16_t) + query.size() + sizeof(uint16_t);

  bufs->push_back(Buffer(buf_size));
  length += buf_size;

  Buffer& buf = bufs->back();
  size_t pos = buf.encode_byte(0, kind());
  pos = buf.encode_long_string(pos, query.data(), query.size());

  if (has_names_for_values()) {
    if (version < 3) {
      LOG_ERROR("Protocol version %d does not support named values", version);
      return ENCODE_ERROR_UNSUPPORTED_PROTOCOL;
    }
    buf.encode_uint16(pos, value_names_.size());
    length += copy_buffers_with_names(bufs);
  } else if (buffers_count() > 0) {
    buf.encode_uint16(pos, buffers_count());
    length += copy_buffers(bufs);
  }

  return length;
}

size_t QueryRequest::get_indices(StringRef name, HashIndex::IndexVec* indices) {
  if (!value_names_index_) {
    set_has_names_for_values(true);
    value_names_index_.reset(new HashIndex(buffers_count()));
  }

  if (value_names_index_->get(name, indices) == 0) {
    size_t index = value_names_.size();

    if (index > buffers_count()) {
    // No more space left for new named values
      return 0;
    }
    value_names_.push_back(ValueName(name.to_string()));

    ValueName* value_name = &value_names_.back();
    value_name->index = index;
    value_name->name = value_name->to_string_ref();
    value_names_index_->insert(value_name);

    indices->push_back(index);
  }

  return indices->size();
}

int32_t QueryRequest::copy_buffers_with_names(BufferVec* bufs) const {
  int32_t size = 0;
  for (size_t i = 0; i < value_names_.size(); ++i) {
    const Buffer& name_buf = value_names_[i].buf;
    bufs->push_back(name_buf);

    const Buffer& value_buf(buffers()[i]);
    bufs->push_back(value_buf);

    size += name_buf.size() + value_buf.size();
  }
  return size;
}

int QueryRequest::encode(int version, BufferVec* bufs, EncodingCache* cache) const {
  if (version == 1) {
    return internal_encode_v1(bufs);
  } else  {
    return encode_internal(version, bufs, cache);
  }
}

int QueryRequest::internal_encode_v1(BufferVec* bufs) const {
  // <query> [long string] + <consistency> [short]
  size_t length = sizeof(int32_t) + query_.size() + sizeof(uint16_t);

  Buffer buf(length);
  size_t pos = buf.encode_long_string(0, query_.data(), query_.size());
  buf.encode_uint16(pos, consistency());
  bufs->push_back(buf);

  return length;
}

int QueryRequest::encode_internal(int version, BufferVec* bufs, EncodingCache* cache) const {
  int length = 0;
  uint8_t flags = this->flags();

    // <query> [long string] + <consistency> [short] + <flags> [byte]
  size_t query_buf_size = sizeof(int32_t) + query_.size() +
                          sizeof(uint16_t) + sizeof(uint8_t);
  size_t paging_buf_size = 0;

  if (elements_count() > 0) { // <values> = <n><value_1>...<value_n>
    query_buf_size += sizeof(uint16_t); // <n> [short]
    flags |= CASS_QUERY_FLAG_VALUES;
  }

  if (page_size() > 0) {
    paging_buf_size += sizeof(int32_t); // [int]
    flags |= CASS_QUERY_FLAG_PAGE_SIZE;
  } else {

  }

  if (!paging_state().empty()) {
    paging_buf_size += sizeof(int32_t) + paging_state().size(); // [bytes]
    flags |= CASS_QUERY_FLAG_PAGING_STATE;
  }

  if (serial_consistency() != 0) {
    paging_buf_size += sizeof(uint16_t); // [short]
    flags |= CASS_QUERY_FLAG_SERIAL_CONSISTENCY;
  }

  {
    bufs->push_back(Buffer(query_buf_size));
    length += query_buf_size;

    Buffer& buf = bufs->back();
    size_t pos = buf.encode_long_string(0, query_.data(), query_.size());
    pos = buf.encode_uint16(pos, consistency());
    pos = buf.encode_byte(pos, flags);

    if (has_names_for_values()) {
      if (version < 3) {
        LOG_ERROR("Protocol version %d does not support named values", version);
        return ENCODE_ERROR_UNSUPPORTED_PROTOCOL;
      }
      buf.encode_uint16(pos, value_names_.size());
      length += copy_buffers_with_names(bufs);
    } else if (buffers_count() > 0) {
      buf.encode_uint16(pos, buffers_count());
      length += copy_buffers(bufs);
    }
  }

  if (paging_buf_size > 0) {
    bufs->push_back(Buffer(paging_buf_size));
    length += paging_buf_size;

    Buffer& buf = bufs->back();
    size_t pos = 0;

    if (page_size() >= 0) {
      pos = buf.encode_int32(pos, page_size());
    }

    if (!paging_state().empty()) {
      pos = buf.encode_bytes(pos, paging_state().data(), paging_state().size());
    }

    if (serial_consistency() != 0) {
      pos = buf.encode_uint16(pos, serial_consistency());
    }
  }

  return length;
}

} // namespace cass
