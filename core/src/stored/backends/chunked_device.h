/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2015-2017 Planets Communications B.V.
   Copyright (C) 2018-2026 Bareos GmbH & Co. KG

   This program is Free Software; you can redistribute it and/or
   modify it under the terms of version three of the GNU Affero General Public
   License as published by the Free Software Foundation, which is
   listed in the file LICENSE.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
   Affero General Public License for more details.

   You should have received a copy of the GNU Affero General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
   02110-1301, USA.
*/
/*
 * Chunked device device abstraction.
 *
 * Marco van Wieringen, February 2015
 */

#ifndef BAREOS_STORED_BACKENDS_CHUNKED_DEVICE_H_
#define BAREOS_STORED_BACKENDS_CHUNKED_DEVICE_H_

#include <sys/types.h>
#include "stored/dev.h"
#include "ordered_cbuf.h"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

template <typename T> class alist;

namespace storagedaemon {

class DeviceControlRecord;
struct DeviceStatusInformation;

// Let io-threads check for work every 300 seconds.
#define DEFAULT_RECHECK_INTERVAL 300

/*
 * Recheck interval when waiting that buffer gets written
 * (write buffer is empty).
 */
#define DEFAULT_RECHECK_INTERVAL_WRITE_BUFFER 10

// Seconds without upload progress after which a flush wait fails.
#define DEFAULT_FLUSH_TIMEOUT 1800

/*
 * Chunk the volume into chunks of this size.
 * This is the lower limit used the exact chunksize is
 * configured as a device option.
 */
#define DEFAULT_CHUNK_SIZE 10 * 1024 * 1024

/*
 * Maximum number of chunks per volume.
 * When you change this make sure you update the %04d format
 * used in the code to format the chunk numbers e.g. 0000-9999
 */
#define MAX_CHUNKS 10000

/*
 * Busy wait retry for inflight chunks.
 * Default 120 * 5 = 600 seconds, 10 minutes.
 */
#define INFLIGHT_RETRIES 120
#define INFLIGT_RETRY_TIME 5
#define NUMBER_OF_RETRIES 5

enum thread_wait_type
{
  WAIT_CANCEL_THREAD, /* Perform a pthread_cancel() on exit. */
  WAIT_JOIN_THREAD    /* Perform a pthread_join() on exit. */
};

// Whether a chunk is in the volume's listing.
enum class ChunkPresence
{
  kPresent,
  kAbsent,
  kUnknown  // the listing failed
};

// What the failed read of a chunk means for a restore.
enum class ReadEnd
{
  kEndOfVolume,  // the volume ends before this chunk
  kShortVolume,  // the catalog counts bytes the volume does not have
  kReadError
};

/* A read that fails ends the volume only when the chunk is absent and starts
 * at or after the catalog size of the volume; anything else is an error. */
constexpr ReadEnd DecideReadEnd(ChunkPresence presence,
                                uint64_t chunk_start,
                                uint64_t volume_bytes)
{
  if (presence != ChunkPresence::kAbsent) { return ReadEnd::kReadError; }
  return chunk_start >= volume_bytes ? ReadEnd::kEndOfVolume
                                     : ReadEnd::kShortVolume;
}

struct thread_handle {
  thread_wait_type type; /* See WAIT_*_THREAD thread_wait_type enum */
  pthread_t thread_id;   /* Actual threadid */
};

struct chunk_io_request {
  const char* volname; /* VolumeName */
  uint16_t chunk;      /* Chunk number */
  char* buffer;        /* Data */
  uint32_t wbuflen;    /* Size of the actual valid data in the chunk (Write) */
  uint32_t* rbuflen;   /* Size of the actual valid data in the chunk (Read) */
  uint8_t tries; /* Number of times the flush was tried to the backing store */
  bool release;  /* Should we release the data to which the buffer points ? */
  int64_t retry_at_ms; /* Steady clock time (ms) before which no retry */
  bool lease_conflict; /* Set by FlushRemoteChunk: chunk is being uploaded */
  bool canceled;       /* Set by FlushRemoteChunk: the job was canceled */
};

struct chunk_descriptor {
  ssize_t chunk_size;     /* Total size of the memory chunk */
  char* buffer;           /* Data */
  uint32_t buflen;        /* Size of the actual valid data in the chunk */
  boffset_t start_offset; /* Start offset of the current chunk */
  boffset_t end_offset;   /* End offset of the current chunk */
  bool need_flushing; /* Data is dirty and needs flushing to backing store */
  bool chunk_setup;   /* Chunk is initialized and ready for use */
  bool writing;       /* We are currently writing */
  bool opened;        /* An open call was done */
};

class InflightChunkException : public std::exception {};

// Outcome of queueing a chunk for upload.
enum class EnqueueResult
{
  kQueued,
  kKeptButRefused,  // queued past the capacity; the write must fail
  kFailed           // not queued
};

/* A chunk of a device without io-threads that is not uploaded yet; it owns
 * its buffer until an upload succeeds. */
struct KeptChunk {
  std::string volname;
  uint16_t chunk{};
  char* buffer{};
  uint32_t buflen{};
  uint8_t tries{};
  int64_t retry_at_ms{};
  bool uploading{};
};

// Snapshot of a device's upload progress, read by a waiting flush.
struct UploadState {
  uint64_t events{};       // finished upload tries
  uint64_t done{};         // successful uploads
  std::string last_error;  // of an upload or a size check
  std::string readonly_reason;
  int64_t outcome_ms{};       // steady clock of the last finished try, 0 = none
  bool outcome_ok{};          // whether that try uploaded
  std::string outcome_error;  // why it failed
};

class ChunkedDevice : public Device {
  friend class ChunkStateProbe;  // unit test of the chunk state
  class InflightLease {
    ChunkedDevice* m_device;
    chunk_io_request* m_request;

   public:
    InflightLease(ChunkedDevice* t_device, chunk_io_request* t_request)
        : m_device(t_device), m_request(t_request)
    {
      if (!m_device->SetInflightChunk(m_request)) {
        throw InflightChunkException();
      }
    }
    ~InflightLease()
    {
      if (m_device && m_request) { m_device->ClearInflightChunk(m_request); }
    }

    InflightLease(const InflightLease&) = delete;
    InflightLease& operator=(const InflightLease&) = delete;

    InflightLease(InflightLease&& other) noexcept
        : m_device(other.m_device), m_request(other.m_request)
    {
      other.m_device = nullptr;
      other.m_request = nullptr;
    };
    InflightLease& operator=(InflightLease&& other) noexcept
    {
      std::swap(m_device, other.m_device);
      std::swap(m_request, other.m_request);
      return *this;
    };
  };

 private:
  // Private Members
  bool io_threads_started_{};
  bool end_of_media_{};
  std::atomic<bool> readonly_{};
  uint8_t inflight_chunks_{};
  char* current_volname_{};
  ordered_circbuf* cb_{};
  alist<thread_handle*>* thread_ids_{};
  chunk_descriptor* current_chunk_{};

  // Finished upload tries wake a waiting flush; guarded by upload_mutex_.
  std::mutex upload_mutex_;
  std::condition_variable upload_cv_;
  UploadState upload_state_;
  // Requests per volume that are queued or held by an io-thread.
  std::map<std::string, int> pending_requests_;

  // Chunks of blocking uploads still to be uploaded; guarded by kept_mutex_.
  std::mutex kept_mutex_;
  std::list<KeptChunk> kept_chunks_;
  std::atomic<size_t> kept_count_{};
  // Set when a refused seek dropped the current chunk: reads and writes fail
  // until a seek loads a chunk or the device is opened again.
  std::atomic<bool> chunk_dropped_{};

  // Private Methods
  char* allocate_chunkbuffer();
  void FreeChunkbuffer(char* buffer);
  void FreeChunkIoRequest(chunk_io_request* request);
  void StopThreads();
  EnqueueResult EnqueueChunk(chunk_io_request* request, std::string& refusal);
  bool RequeueChunk(chunk_io_request* request);
  bool FlushChunk(bool release_chunk, bool move_to_next_chunk);
  bool FlushChunkCopy(std::string& refusal);
  bool ReadChunk();
  bool is_written();
  void NotifyUploadEvent(bool uploaded, const std::string& error);
  void SetLastError(const std::string& error);
  void SetReadonly(const std::string& reason);
  void ClearReadonlyIfDrained();
  UploadState GetUploadState();
  void CountPendingRequest(const char* volname, int change);
  int PendingChunksOfVolume(const char* volname);
  void KeepCurrentChunk(bool failed, const std::string& error);
  bool UploadKeptChunk(std::list<KeptChunk>::iterator entry);
  void TryKeptChunks(bool only_due, size_t limit = SIZE_MAX);
  void ClearKeptReadonly();

 protected:
  // Protected Members
  /* Guards current_chunk_; the release flush runs without the device lock.
   * Lock order: device lock, chunk_mutex_, kept_mutex_, upload_mutex_. */
  std::recursive_mutex chunk_mutex_;
  uint8_t io_threads_{};
  uint8_t io_slots_{};
  uint8_t retries_{};
  uint32_t flush_timeout_{DEFAULT_FLUSH_TIMEOUT};
  uint64_t chunk_size_{};
  boffset_t offset_{};
  bool use_mmap_{};

  // Protected Methods
  void FailWriter(const char* message);
  std::optional<InflightLease> getInflightLease(chunk_io_request* request);
  bool SetInflightChunk(chunk_io_request* request);
  void ClearInflightChunk(chunk_io_request* request);
  bool IsInflightChunk(chunk_io_request* request);
  int NrInflightChunks();
  int SetupChunk(const char* pathname, int flags, int mode);
  ssize_t ReadChunked(int fd, void* buffer, size_t count);
  ssize_t WriteChunked(int fd, const void* buffer, size_t count);
  int CloseChunk();
  bool TruncateChunkedVolume(DeviceControlRecord* dcr);
  ssize_t ChunkedVolumeSize();
  bool LoadChunk();
  void InvalidateCurrentChunk();
  bool ReadEndsTheVolume();
  bool CurrentChunkNeedsFlushing();
  bool StartEmptyChunkAfterLast(const std::map<int, size_t>& chunk_sizes);
  bool WaitForPendingChunks(const std::function<bool()>& is_canceled,
                            std::string& reason);
  bool WaitUntilChunksWritten(const std::function<bool()>& is_canceled,
                              std::string& reason);
  std::vector<std::pair<std::string, uint16_t>> ChunksNotUploaded();
  // Virtual so that tests can cancel the writer or fail the thread start.
  virtual bool WriterCanceled();
  virtual bool StartIoThreads();

  // Methods implemented by inheriting class.
  virtual bool CheckRemoteConnection() = 0;
  virtual bool FlushRemoteChunk(chunk_io_request* request) = 0;
  virtual bool ReadRemoteChunk(chunk_io_request* request) = 0;
  // Volume size, -1 when it has no chunks, nullopt (errmsg set) on error.
  virtual std::optional<ssize_t> RemoteVolumeSize() = 0;
  virtual bool TruncateRemoteVolume(DeviceControlRecord* dcr) = 0;
  // Whether the chunk is in a fresh listing; unknown ends a failed read as an
  // error.
  virtual ChunkPresence RemoteChunkPresence(int chunk)
  {
    (void)chunk;
    return ChunkPresence::kUnknown;
  }

 public:
  // Public Methods
  ChunkedDevice() = default;
  virtual ~ChunkedDevice();

  bool DequeueChunk();
  bool DeviceStatus(DeviceStatusInformation* dst) override;
};

} /* namespace storagedaemon */

#endif  // BAREOS_STORED_BACKENDS_CHUNKED_DEVICE_H_
