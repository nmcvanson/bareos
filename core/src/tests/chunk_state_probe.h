/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2026-2026 Bareos GmbH & Co. KG

   This program is Free Software; you can redistribute it and/or
   modify it under the terms of version three of the GNU Affero General Public
   License as published by the Free Software Foundation and included
   in the file LICENSE.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
   Affero General Public License for more details.

   You should have received a copy of the GNU Affero General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
   02110-1301, USA.
*/

// Test access to the current chunk of a ChunkedDevice (friend of the class).
#ifndef BAREOS_TESTS_CHUNK_STATE_PROBE_H_
#define BAREOS_TESTS_CHUNK_STATE_PROBE_H_

#include <string>
#include "stored/backends/chunked_device.h"

namespace storagedaemon {

// Everything the device keeps about its current chunk.
struct ChunkState {
  boffset_t start_offset, end_offset, offset;
  uint32_t buflen;
  bool need_flushing, chunk_setup, writing, opened;
  int64_t chunk_number;
  std::string bytes;

  bool operator==(const ChunkState& o) const
  {
    return start_offset == o.start_offset && end_offset == o.end_offset
           && offset == o.offset && buflen == o.buflen
           && need_flushing == o.need_flushing && chunk_setup == o.chunk_setup
           && writing == o.writing && opened == o.opened
           && chunk_number == o.chunk_number && bytes == o.bytes;
  }
};

class ChunkStateProbe {
 public:
  static ChunkState Capture(ChunkedDevice& dev)
  {
    const chunk_descriptor* c = dev.current_chunk_;
    ChunkState state{};
    state.offset = dev.offset_;
    if (!c) { return state; }
    state.start_offset = c->start_offset;
    state.end_offset = c->end_offset;
    state.buflen = c->buflen;
    state.need_flushing = c->need_flushing;
    state.chunk_setup = c->chunk_setup;
    state.writing = c->writing;
    state.opened = c->opened;
    state.chunk_number
        = c->chunk_size > 0 ? c->start_offset / c->chunk_size : -1;
    if (c->buffer) { state.bytes.assign(c->buffer, c->chunk_size); }
    return state;
  }

  // Counts a chunk of the volume as pending, as an enqueue (+1) or a finished
  // upload (-1) does.
  static void CountPending(ChunkedDevice& dev, const char* volume, int change)
  { dev.CountPendingRequest(volume, change); }
};

}  // namespace storagedaemon

#endif  // BAREOS_TESTS_CHUNK_STATE_PROBE_H_
