/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2015-2017 Planets Communications B.V.
   Copyright (C) 2017-2026 Bareos GmbH & Co. KG

   This program is Free Software; you can redistribute it and/or
   modify it under the terms of version three of the GNU Affero General Public
   License as published by the Free Software Foundation and included
   in the file LICENSE.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
   General Public License for more details.

   You should have received a copy of the GNU Affero General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
   02110-1301, USA.
*/
/*
 * Chunked volume device abstraction.
 *
 * Marco van Wieringen, February 2015
 */

#if !defined(HAVE_MSVC)
#  include <unistd.h>
#endif

#include "include/fcntl_def.h"
#include "include/bareos.h"
#include "lib/edit.h"
#include "stored/device_status_information.h"

#include "stored/stored.h"
#include "stored/device_control_record.h"
#include "chunked_device.h"
#include "flush_wait.h"

#include "stored/stored_globals.h"
#include "lib/thread_specific_data.h"

#include <chrono>
#include <vector>

namespace storagedaemon {

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

// Copy of a device message without its trailing newlines.
static std::string MessageText(const char* message)
{
  std::string text{message ? message : ""};
  while (!text.empty() && text.back() == '\n') { text.pop_back(); }
  return text;
}

// Milliseconds of the steady clock, for the retry times kept in requests.
static int64_t SteadyMilliseconds()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Realtime clock value after the given wait, for timed condition waits.
static struct timespec AbsoluteTimeAfter(std::chrono::milliseconds wait)
{
  struct timeval tv;
  struct timespec ts;

  gettimeofday(&tv, NULL);
  const int64_t nsec = static_cast<int64_t>(tv.tv_usec) * 1000
                       + (wait.count() % 1000) * 1000000;
  ts.tv_sec = tv.tv_sec + wait.count() / 1000 + nsec / 1000000000;
  ts.tv_nsec = nsec % 1000000000;
  return ts;
}

/*
 * This implements a device abstraction that provides so called chunked
 * volumes. These chunks are kept in memory and flushed to the backing
 * store when requested. This class fully abstracts the chunked volumes
 * for the upper level device. The stacking for this device type is:
 *
 * <actual_device_type>::
 *          |
 *          v
 *   ChunkedDevice::
 *          |
 *          v
 *       Device::
 *
 * The public interfaces exported from this device are:
 *
 * SetInflightChunk() - Set the inflight flag for a chunk.
 * ClearInflightChunk() - Clear the inflight flag for a chunk.
 * IsInflightChunk() - Is a chunk current inflight to the backing store.
 * NrInflightChunks() - Number of chunks inflight to the backing store.
 * SetupChunk() - Setup a chunked volume for reading or writing.
 * ReadChunked() - Read a chunked volume.
 * WriteChunked() - Write a chunked volume.
 * CloseChunk() - Close a chunked volume.
 * TruncateChunkedVolume() - Truncate a chunked volume.
 * ChunkedVolumeSize() - Get the current size of a volume.
 * LoadChunk() - Make sure we have the right chunk in memory.
 *
 * It also demands that the inheriting class implements the
 * following methods:
 *
 * FlushRemoteChunk() - Flush a chunk to the remote backing store.
 * ReadRemoteChunk() - Read a chunk from the remote backing store.
 * RemoteVolumeSize - Return the current size of a volume.
 * TruncateRemoteVolume() - Truncate a chunked volume on the
 *                                    remote backing store.
 */

// Actual thread runner that processes IO request from circular buffer.
static void* io_thread(void* data)
{
  char ed1[50];
  ChunkedDevice* dev = (ChunkedDevice*)data;

  // Dequeue from the circular buffer until we are done.
  while (1) {
    if (!dev->DequeueChunk()) { break; }
  }

  Dmsg1(100, "Stopping IO-thread threadid=%s\n",
        edit_pthread(pthread_self(), ed1, sizeof(ed1)));

  return NULL;
}

// Allocate a new chunk buffer.
char* ChunkedDevice::allocate_chunkbuffer()
{
  char* buffer = (char*)malloc(current_chunk_->chunk_size);

  Dmsg2(100, "New allocated buffer of %zd bytes at %p\n",
        current_chunk_->chunk_size, buffer);

  return buffer;
}

// Free a chunk buffer.
void ChunkedDevice::FreeChunkbuffer(char* buffer)
{
  Dmsg2(100, "Freeing buffer of %zd bytes at %p\n", current_chunk_->chunk_size,
        buffer);

  free(buffer);
}

// Free a chunk_io_request.
void ChunkedDevice::FreeChunkIoRequest(chunk_io_request* request)
{
  Dmsg2(100, "Freeing chunk io request of %zu bytes at %p\n",
        sizeof(chunk_io_request), request);

  if (request->release) { FreeChunkbuffer(request->buffer); }
  free((void*)request->volname);
  free(request);
}

// Start the io-threads that are used for uploading.
bool ChunkedDevice::StartIoThreads()
{
  char ed1[50];
  uint8_t thread_nr;
  pthread_t thread_id;
  thread_handle* handle;

  /* Create a new ordered circular buffer for exchanging chunks between
   * the producer (the storage driver) and multiple consumers (io-threads). */
  if (io_slots_) {
    cb_ = new storagedaemon::ordered_circbuf(io_threads_ * io_slots_);
  } else {
    cb_ = new storagedaemon::ordered_circbuf(io_threads_ * OQSIZE);
  }

  // Start all IO threads and keep track of their thread ids in thread_ids_.
  if (!thread_ids_) {
    thread_ids_ = new alist<thread_handle*>(10, owned_by_alist);
  }

  for (thread_nr = 1; thread_nr <= io_threads_; thread_nr++) {
    if (pthread_create(&thread_id, NULL, io_thread, (void*)this)) {
      return false;
    }

    handle = (thread_handle*)malloc(sizeof(thread_handle));
    memset(handle, 0, sizeof(thread_handle));
    handle->type = WAIT_JOIN_THREAD;
    memcpy(&handle->thread_id, &thread_id, sizeof(pthread_t));
    thread_ids_->append(handle);

    Dmsg1(100, "Started new IO-thread threadid=%s\n",
          edit_pthread(thread_id, ed1, sizeof(ed1)));
  }

  io_threads_started_ = true;

  return true;
}

// Stop the io-threads that are used for uploading.
void ChunkedDevice::StopThreads()
{
  char ed1[50];

  /* Tell all IO threads that we flush the circular buffer.
   * As such they will get a NULL chunk_io_request back and exit. */
  cb_->flush();

  // Wait for all threads to exit.
  if (thread_ids_) {
    for (auto* handle : thread_ids_) {
      switch (handle->type) {
        case WAIT_CANCEL_THREAD:
          Dmsg1(100, "Canceling thread with threadid=%s\n",
                edit_pthread(handle->thread_id, ed1, sizeof(ed1)));
          pthread_cancel(handle->thread_id);
          break;
        case WAIT_JOIN_THREAD:
          Dmsg1(100, "Waiting to join with threadid=%s\n",
                edit_pthread(handle->thread_id, ed1, sizeof(ed1)));
          pthread_join(handle->thread_id, NULL);
          break;
        default:
          break;
      }
    }

    thread_ids_->destroy();
    delete thread_ids_;
    thread_ids_ = NULL;
  }
}

auto ChunkedDevice::getInflightLease(chunk_io_request* request)
    -> std::optional<InflightLease>
{
  try {
    return InflightLease(this, request);
  } catch (const InflightChunkException&) {
    return std::nullopt;
  }
}

// Set the inflight flag for a chunk.
bool ChunkedDevice::SetInflightChunk(chunk_io_request* request)
{
  PoolMem inflight_file(PM_FNAME);

  Mmsg(inflight_file, "%s/%s@%04d", me->working_directory, request->volname,
       request->chunk);
  PmStrcat(inflight_file, "%inflight");

  Dmsg3(100, "Creating inflight file %s for volume %s, chunk %d\n",
        inflight_file.c_str(), request->volname, request->chunk);

  int inflight_fd
      = ::open(inflight_file.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0640);
  if (inflight_fd >= 0) {
    lock_mutex(mutex);
    inflight_chunks_++;
    unlock_mutex(mutex);
    ::close(inflight_fd);
  } else {
    return false;
  }

  return true;
}

// Clear the inflight flag for a chunk.
void ChunkedDevice::ClearInflightChunk(chunk_io_request* request)
{
  struct stat st;
  PoolMem inflight_file(PM_FNAME);

  if (request) {
    Mmsg(inflight_file, "%s/%s@%04d", me->working_directory, request->volname,
         request->chunk);
    PmStrcat(inflight_file, "%inflight");

    Dmsg3(100, "Removing inflight file %s for volume %s, chunk %d\n",
          inflight_file.c_str(), request->volname, request->chunk);

    if (stat(inflight_file.c_str(), &st) != 0) { return; }

    ::unlink(inflight_file.c_str());
  }

  lock_mutex(mutex);
  inflight_chunks_--;
  unlock_mutex(mutex);
}

// Check if a certain chunk is inflight to the backing store.
bool ChunkedDevice::IsInflightChunk(chunk_io_request* request)
{
  struct stat st;
  PoolMem inflight_file(PM_FNAME);

  Mmsg(inflight_file, "%s/%s@%04d", me->working_directory, request->volname,
       request->chunk);
  PmStrcat(inflight_file, "%inflight");

  if (stat(inflight_file.c_str(), &st) == 0) { return true; }

  return false;
}

// Number of inflight chunks to the backing store.
int ChunkedDevice::NrInflightChunks()
{
  int retval = 0;

  lock_mutex(mutex);
  retval = inflight_chunks_;
  unlock_mutex(mutex);

  return retval;
}

// Call back function for comparing two chunk_io_requests.
static int CompareChunkIoRequest(ocbuf_item* ocbuf1, ocbuf_item* ocbuf2)
{
  chunk_io_request* chunk1 = (chunk_io_request*)ocbuf1->data;
  chunk_io_request* chunk2 = (chunk_io_request*)ocbuf2->data;

  // Same volume name ?
  if (bstrcmp(chunk1->volname, chunk2->volname)) {
    // Compare on chunk number.
    if (chunk1->chunk == chunk2->chunk) {
      return 0;
    } else {
      return (chunk1->chunk < chunk2->chunk) ? -1 : 1;
    }
  } else {
    return strcmp(chunk1->volname, chunk2->volname);
  }
}

// Call back function for updating two chunk_io_requests.
static void UpdateChunkIoRequest(void* old_item, void* new_item)
{
  chunk_io_request* old_req = (chunk_io_request*)old_item;
  chunk_io_request* new_req = (chunk_io_request*)new_item;

  /* See if the new chunk_io_request has more bytes then
   * the chunk_io_request currently on the ordered circular
   * buffer. We can only have multiple chunk_io_requests for
   * the same chunk of a volume when a chunk was not fully
   * filled by one backup Job and a next one writes data to
   * the chunk before its being flushed to backing store. This
   * means all pointers are the same only the wbuflen and the
   * release flag of the chunk_io_request differ. So we only
   * copy those two fields and not the others. */
  Dmsg0(200, "Updating chunk request at %p from new request at %p\n", old_req,
        new_req);
  ASSERT(new_req->wbuflen >= old_req->wbuflen);
  if (new_req->buffer == old_req->buffer) {
    old_req->wbuflen = new_req->wbuflen;
    old_req->release = new_req->release;
    new_req->release = false;
  } else {
    std::swap(*old_req, *new_req);
  }
}

/* Call back function for putting a failed request back while a newer request
 * for the same chunk is queued: the larger one is kept. */
static void UpdateRequeuedChunkIoRequest(void* old_item, void* new_item)
{
  chunk_io_request* old_req = (chunk_io_request*)old_item;
  chunk_io_request* new_req = (chunk_io_request*)new_item;

  if (new_req->wbuflen >= old_req->wbuflen) {
    UpdateChunkIoRequest(old_item, new_item);
  }
}

/* Queues a chunk for upload. While the queue is full the writer waits; on a
 * cancel, a read-only device or no upload progress for flush_timeout_ seconds
 * the chunk is queued past the capacity and the write refused. */
EnqueueResult ChunkedDevice::EnqueueChunk(chunk_io_request* request,
                                          std::string& refusal)
{
  using clock = FlushWaitTracker::clock;
  chunk_io_request *new_request, *enqueued_request;
  EnqueueResult result = EnqueueResult::kQueued;
  bool ignore_full = false;

  Dmsg2(100, "Enqueueing chunk %" PRIu16 " of volume %s (%" PRIu32 " bytes)\n",
        request->chunk, request->volname, request->wbuflen);

  if (!io_threads_started_) {
    if (!StartIoThreads()) {
      refusal = T_("the upload threads could not be started");
      return EnqueueResult::kFailed;
    }
  }

  new_request = (chunk_io_request*)malloc(sizeof(chunk_io_request));
  memset(new_request, 0, sizeof(chunk_io_request));
  new_request->volname = strdup(request->volname);
  new_request->chunk = request->chunk;
  new_request->buffer = request->buffer;
  new_request->wbuflen = request->wbuflen;
  new_request->tries = 0;
  new_request->release = request->release;

  Dmsg2(100, "Allocated chunk io request of %" PRIuz " bytes at %p\n",
        sizeof(chunk_io_request), new_request);

  // Counted before an io-thread can take it, uncounted if it is not kept.
  CountPendingRequest(request->volname, 1);
  FlushWaitTracker tracker{std::chrono::seconds{flush_timeout_}, clock::now(),
                           GetUploadState().done};
  while (true) {
    const struct timespec slice = AbsoluteTimeAfter(std::chrono::seconds{1});
    bool was_full = false;

    /* Enqueue the item onto the ordered circular buffer.
     * This returns either the same request as we passed
     * in or the previous flush request for the same chunk. */
    enqueued_request = (chunk_io_request*)cb_->enqueue(
        new_request, sizeof(chunk_io_request), CompareChunkIoRequest,
        UpdateChunkIoRequest, false, /* use_reserved_slot */
        false /* no_signal */, &slice, &was_full, ignore_full);
    if (!was_full) { break; }

    const UploadState uploads = GetUploadState();
    PoolMem message(PM_MESSAGE);
    switch (tracker.Check(false, WriterCanceled(), readonly_, uploads.done,
                          clock::now())) {
      case FlushWaitResult::kWritten:
      case FlushWaitResult::kWaiting:
        continue;
      case FlushWaitResult::kCanceled:
        refusal = T_("job canceled while waiting for a free upload slot");
        break;
      case FlushWaitResult::kReadOnly:
        refusal = uploads.readonly_reason;
        break;
      case FlushWaitResult::kTimedOut:
        Mmsg(message,
             T_("no upload progress for %" PRIu32
                " seconds while the upload queue was full; the chunks stay "
                "queued. Last backend error: %s"),
             flush_timeout_,
             uploads.last_error.empty() ? "none" : uploads.last_error.c_str());
        refusal = message.c_str();
        Emsg2(M_ERROR, 0, T_("Setting device %s readonly: %s.\n"), print_name(),
              refusal.c_str());
        SetReadonly(refusal);
        break;
    }
    ignore_full = true;
    result = EnqueueResult::kKeptButRefused;
  }

  if (!enqueued_request) {
    // The caller keeps the chunk's buffer.
    CountPendingRequest(request->volname, -1);
    new_request->release = false;
    FreeChunkIoRequest(new_request);
    refusal = T_("the upload queue did not take the chunk");
    return EnqueueResult::kFailed;
  }

  // Compare the return value from the enqueue.
  if (enqueued_request != new_request) {
    CountPendingRequest(request->volname, -1);
    FreeChunkIoRequest(new_request);
  }

  return result;
}

/* Puts a request taken by dequeue() back into its reserved slot without
 * waking the other io-threads; false when the buffer did not take it. */
bool ChunkedDevice::RequeueChunk(chunk_io_request* request)
{
  chunk_io_request* enqueued_request = (chunk_io_request*)cb_->enqueue(
      request, sizeof(chunk_io_request), CompareChunkIoRequest,
      UpdateRequeuedChunkIoRequest, true, /* use_reserved_slot */
      true /* no_signal */);
  if (!enqueued_request) {
    Dmsg2(100, "Error: Chunk %d of volume %s not appended to queue\n",
          request->chunk, request->volname);
    return false;
  }

  /* If it is different there was already a chunk io request for the same
   * chunk on the ordered circular buffer. */
  if (enqueued_request != request) {
    Dmsg2(100, "Attempted to append chunk %d of volume %s twice\n",
          request->chunk, request->volname);
    CountPendingRequest(request->volname, -1);
    FreeChunkIoRequest(request);
  }
  return true;
}

/*
 * Dequeue a chunk flush request from the ordered circular buffer and process
 * it.
 */
bool ChunkedDevice::DequeueChunk()
{
  char ed1[50];
  struct timespec ts;
  bool requeued = false;
  std::chrono::milliseconds retry_pause{0};
  chunk_io_request* new_request;

  /* Loop while we are not done either due to the ordered circular buffer being
   * flushed some fatal error or successfully dequeueing a chunk flush request.
   */
  while (1) {
    /* See if we are in the flushing state then we just return and exit the
     * io-thread. */
    if (cb_->IsFlushing()) { return false; }

    /* Calculate the next absolute timeout if we find out there is no work to be
     * done; after putting a chunk back, wait for its retry pause. */
    ts = AbsoluteTimeAfter(requeued
                               ? retry_pause
                               : std::chrono::milliseconds{std::chrono::seconds{
                                     DEFAULT_RECHECK_INTERVAL}});

    /* Dequeue the next item from the ordered circular buffer and reserve the
     * slot as we might need to put this item back onto the ordered circular
     * buffer if we fail to flush it to the remote backing store. */
    new_request = (chunk_io_request*)cb_->dequeue(
        true,     /* reserve_slot we may need to enqueue the request */
        requeued, /* request is requeued due to failure ? */
        &ts, DEFAULT_RECHECK_INTERVAL);
    if (!new_request) { return false; }

    // A chunk whose retry is not due yet goes back; this thread waits for it.
    const int64_t now_ms = SteadyMilliseconds();
    if (new_request->retry_at_ms > now_ms) {
      retry_pause
          = std::chrono::milliseconds{new_request->retry_at_ms - now_ms};
      if (!RequeueChunk(new_request)) { return false; }
      requeued = true;
      continue;
    }

    Dmsg3(100, "Flushing chunk %d of volume %s by thread %s\n",
          new_request->chunk, new_request->volname,
          edit_pthread(pthread_self(), ed1, sizeof(ed1)));

    if (!FlushRemoteChunk(new_request)) {
      const std::string error = MessageText(errmsg);

      /* A chunk that used up its maximum number of tries sets the device
       * read-only, so that further writes fail. The chunk is kept and
       * retried below until it is uploaded. */
      if (new_request->tries < UINT8_MAX) { new_request->tries++; }
      if (retries_ > 0 && new_request->tries == retries_) {
        PoolMem reason(PM_MESSAGE);
        Mmsg(reason,
             T_("chunk %d of volume %s could not be uploaded after %d tries: "
                "%s"),
             new_request->chunk, new_request->volname, new_request->tries,
             error.c_str());
        Emsg2(M_ERROR, 0,
              T_("Setting device %s readonly: %s. The chunk stays queued and "
                 "is retried.\n"),
              print_name(), reason.c_str());
        SetReadonly(reason.c_str());
      }

      /* The chunk goes back without waking the other io-threads and is tried
       * again after a pause that grows with its number of tries. */
      retry_pause = UploadRetryPause(new_request->tries,
                                     std::chrono::seconds{flush_timeout_});
      new_request->retry_at_ms = SteadyMilliseconds() + retry_pause.count();
      Dmsg3(100, "Enqueueing chunk %d of volume %s for retry in %lld ms\n",
            new_request->chunk, new_request->volname,
            static_cast<long long>(retry_pause.count()));
      if (!RequeueChunk(new_request)) { return false; }

      NotifyUploadEvent(false, error);
      requeued = true;
      continue;
    }

    /* Uncounted before the slot reserved by dequeue() is given back, so a
     * drained buffer means no chunk is counted any more. */
    CountPendingRequest(new_request->volname, -1);
    cb_->unreserve_slot();

    // Processed the chunk so clean it up now.
    FreeChunkIoRequest(new_request);

    ClearReadonlyIfDrained();
    NotifyUploadEvent(true, {});

    return true;
  }
}

// Records a finished upload try and wakes a waiting flush.
void ChunkedDevice::NotifyUploadEvent(bool uploaded, const std::string& error)
{
  {
    std::lock_guard<std::mutex> lock(upload_mutex_);
    ++upload_state_.events;
    if (uploaded) {
      ++upload_state_.done;
    } else {
      upload_state_.last_error = error;
    }
  }
  upload_cv_.notify_all();
}

// Keeps the last backend error for the message of a failed flush wait.
void ChunkedDevice::SetLastError(const std::string& error)
{
  std::lock_guard<std::mutex> lock(upload_mutex_);
  upload_state_.last_error = error;
}

// Makes further writes fail; the reason is reported to the jobs.
void ChunkedDevice::SetReadonly(const std::string& reason)
{
  {
    std::lock_guard<std::mutex> lock(upload_mutex_);
    upload_state_.readonly_reason = reason;
  }
  readonly_ = true;
}

// Lets the device take writes again once nothing is queued or inflight.
void ChunkedDevice::ClearReadonlyIfDrained()
{
  if (!readonly_ || !cb_->empty_with_no_reserve() || NrInflightChunks() > 0) {
    return;
  }
  bool expected = true;
  if (readonly_.compare_exchange_strong(expected, false)) {
    {
      std::lock_guard<std::mutex> lock(upload_mutex_);
      upload_state_.readonly_reason.clear();
    }
    Emsg1(M_INFO, 0,
          T_("All queued chunks of device %s are uploaded, it takes writes "
             "again.\n"),
          print_name());
  }
}

UploadState ChunkedDevice::GetUploadState()
{
  std::lock_guard<std::mutex> lock(upload_mutex_);
  return upload_state_;
}

// Whether the job of the calling thread was canceled or has failed.
bool ChunkedDevice::WriterCanceled()
{
  JobControlRecord* jcr = GetJcrFromThreadSpecificData();
  return jcr != nullptr && jcr->IsJobCanceled();
}

/* Fails the job of the calling thread with the message. The status is set
 * directly, as Jmsg skips it when no destination takes fatal messages. */
void ChunkedDevice::FailWriter(const char* message)
{
  JobControlRecord* jcr = GetJcrFromThreadSpecificData();
  Jmsg(jcr, M_FATAL, 0, "%s", message);
  if (jcr) { jcr->setJobStatusWithPriorityCheck(JS_FatalError); }
}

// Adds change to the number of queued or uploading requests of the volume.
void ChunkedDevice::CountPendingRequest(const char* volname, int change)
{
  std::lock_guard<std::mutex> lock(upload_mutex_);
  int& count = pending_requests_[volname];
  count += change;
  if (count == 0) { pending_requests_.erase(volname); }
}

/* Number of chunks of the volume that this device has queued or is uploading,
 * including a chunk an io-thread holds between two tries. */
int ChunkedDevice::PendingChunksOfVolume(const char* volname)
{
  std::lock_guard<std::mutex> lock(upload_mutex_);
  auto it = pending_requests_.find(volname);
  return it == pending_requests_.end() ? 0 : it->second;
}

// Read-only reason of a kept chunk whose upload failed.
static std::string KeptChunkFailure(uint16_t chunk,
                                    const std::string& volname,
                                    const std::string& error)
{
  PoolMem reason(PM_MESSAGE);
  Mmsg(reason, T_("chunk %d of volume %s could not be uploaded: %s"), chunk,
       volname.c_str(), error.c_str());
  return reason.c_str();
}

/* Without io-threads: puts the current chunk on the kept list. A failed chunk
 * moves its buffer there and makes the device read-only; a release flush
 * keeps a copy. Caller holds chunk_mutex_. */
void ChunkedDevice::KeepCurrentChunk(bool failed, const std::string& error)
{
  KeptChunk entry;
  entry.volname = current_volname_;
  entry.chunk = current_chunk_->start_offset / current_chunk_->chunk_size;
  entry.buflen = current_chunk_->buflen;
  if (failed) {
    entry.buffer = current_chunk_->buffer;
    entry.tries = 1;
    entry.retry_at_ms
        = SteadyMilliseconds()
          + UploadRetryPause(1, std::chrono::seconds{flush_timeout_}).count();
  } else {
    entry.buffer = allocate_chunkbuffer();
    memcpy(entry.buffer, current_chunk_->buffer, current_chunk_->buflen);
  }

  // Listed and read-only before the chunk leaves current_chunk_.
  size_t kept = 0;
  {
    std::lock_guard<std::mutex> lock(kept_mutex_);
    if (failed) {
      SetReadonly(KeptChunkFailure(entry.chunk, entry.volname, error));
    }
    kept_chunks_.push_back(std::move(entry));
    CountPendingRequest(current_volname_, 1);
    kept = ++kept_count_;
  }
  if (kept > 2) {
    Emsg2(M_WARNING, 0, T_("Device %s keeps %d chunks waiting for upload.\n"),
          print_name(), static_cast<int>(kept));
  }

  if (failed) {
    current_chunk_->buffer = allocate_chunkbuffer();
    current_chunk_->buflen = 0;
  }
  current_chunk_->need_flushing = false;
}

/* Uploads a kept chunk that the caller marked as uploading, without holding
 * kept_mutex_; true when it is stored. */
bool ChunkedDevice::UploadKeptChunk(std::list<KeptChunk>::iterator entry)
{
  chunk_io_request request{};
  request.volname = entry->volname.c_str();
  request.chunk = entry->chunk;
  request.buffer = entry->buffer;
  request.wbuflen = entry->buflen;

  const bool uploaded = FlushRemoteChunk(&request);
  const std::string error = uploaded ? std::string{} : MessageText(errmsg);
  {
    std::lock_guard<std::mutex> lock(kept_mutex_);
    if (uploaded) {
      CountPendingRequest(entry->volname.c_str(), -1);
      FreeChunkbuffer(entry->buffer);
      kept_chunks_.erase(entry);
      if (--kept_count_ == 0) { ClearKeptReadonly(); }
    } else {
      if (entry->tries < UINT8_MAX) { entry->tries++; }
      entry->retry_at_ms
          = SteadyMilliseconds()
            + UploadRetryPause(entry->tries,
                               std::chrono::seconds{flush_timeout_})
                  .count();
      entry->uploading = false;
      SetReadonly(KeptChunkFailure(entry->chunk, entry->volname, error));
    }
  }
  NotifyUploadEvent(uploaded, error);
  return uploaded;
}

/* Gives every idle kept chunk, or every one whose retry is due, one upload
 * try; no pause and no lock is held across a try. */
void ChunkedDevice::TryKeptChunks(bool only_due)
{
  std::vector<std::list<KeptChunk>::iterator> taken;
  {
    std::lock_guard<std::mutex> lock(kept_mutex_);
    const int64_t now_ms = SteadyMilliseconds();
    for (auto it = kept_chunks_.begin(); it != kept_chunks_.end(); ++it) {
      if (it->uploading || (only_due && it->retry_at_ms > now_ms)) { continue; }
      it->uploading = true;
      taken.push_back(it);
    }
  }
  for (auto entry : taken) {
    Dmsg2(100, "Uploading kept chunk %d of volume %s\n", entry->chunk,
          entry->volname.c_str());
    UploadKeptChunk(entry);
  }
}

// Lets the device take writes again; caller holds kept_mutex_.
void ChunkedDevice::ClearKeptReadonly()
{
  if (!readonly_) { return; }
  {
    std::lock_guard<std::mutex> lock(upload_mutex_);
    upload_state_.readonly_reason.clear();
  }
  readonly_ = false;
  Emsg1(M_INFO, 0,
        T_("All kept chunks of device %s are uploaded, it takes writes "
           "again.\n"),
        print_name());
}

/* Queues the current chunk for the io-threads, or without them uploads it once.
 * When the chunk is refused, cannot be queued or fails to upload, the job of
 * the writer fails; a chunk that could not be queued stays in the buffer, a
 * failed upload is kept. Caller holds chunk_mutex_. */
bool ChunkedDevice::FlushChunk(bool release_chunk, bool move_to_next_chunk)
{
  bool retval = false;
  chunk_io_request request;

  // Calculate in which chunk we are currently.
  request.chunk = current_chunk_->start_offset / current_chunk_->chunk_size;
  request.volname = current_volname_;
  request.buffer = current_chunk_->buffer;
  request.wbuflen = current_chunk_->buflen;
  request.release = release_chunk;

  if (io_threads_) {
    std::string refusal;
    PoolMem message(PM_MESSAGE);
    switch (EnqueueChunk(&request, refusal)) {
      case EnqueueResult::kQueued:
        retval = true;
        break;
      case EnqueueResult::kKeptButRefused:
        Mmsg(message, T_("Device %s refuses writes: %s\n"), print_name(),
             refusal.c_str());
        FailWriter(message.c_str());
        break;
      case EnqueueResult::kFailed:
        // The chunk stays in the device's buffer, still to be flushed.
        Mmsg(message,
             T_("Device %s could not queue chunk %d of volume %s: %s\n"),
             print_name(), request.chunk, request.volname, refusal.c_str());
        FailWriter(message.c_str());
        return false;
    }
  } else {
    // no multithreading
    Dmsg1(100, "Try to flush chunk number: %d\n", request.chunk);
    retval = FlushRemoteChunk(&request);
    if (retval) {
      NotifyUploadEvent(true, {});
    } else {
      // A chunk with data is kept for a later upload, never dropped.
      const std::string error = MessageText(errmsg);
      PoolMem message(PM_MESSAGE);
      Mmsg(message,
           T_("Device %s could not upload chunk %d of volume %s: %s\n"),
           print_name(), request.chunk, request.volname, error.c_str());
      if (request.wbuflen > 0) {
        KeepCurrentChunk(true, error);
        NotifyUploadEvent(false, error);
      }
      FailWriter(message.c_str());
    }
  }

  // Clear the need flushing flag.
  current_chunk_->need_flushing = false;

  // Change to the next chunk ?
  if (move_to_next_chunk) {
    // If we enqueued the data we need to allocate a new buffer.
    if (io_threads_) { current_chunk_->buffer = allocate_chunkbuffer(); }
    current_chunk_->start_offset += current_chunk_->chunk_size;
    current_chunk_->end_offset
        = current_chunk_->start_offset + (current_chunk_->chunk_size - 1);
    current_chunk_->buflen = 0;
  } else {
    // If we enqueued the data we need to allocate a new buffer.
    if (release_chunk && io_threads_) { current_chunk_->buffer = NULL; }
  }

  if (!retval) { Dmsg1(100, "%s", errmsg); }

  return retval;
}

/* Enqueues a copy of the current chunk and keeps the device's buffer, so a
 * queued upload never points into a buffer the device frees or reuses.
 * Caller holds chunk_mutex_. */
bool ChunkedDevice::FlushChunkCopy(std::string& refusal)
{
  chunk_io_request request{};

  request.chunk = current_chunk_->start_offset / current_chunk_->chunk_size;
  request.volname = current_volname_;
  request.buffer = allocate_chunkbuffer();
  memcpy(request.buffer, current_chunk_->buffer, current_chunk_->buflen);
  request.wbuflen = current_chunk_->buflen;
  request.release = true;

  switch (EnqueueChunk(&request, refusal)) {
    case EnqueueResult::kQueued:
      current_chunk_->need_flushing = false;
      return true;
    case EnqueueResult::kKeptButRefused:
      current_chunk_->need_flushing = false;
      return false;
    case EnqueueResult::kFailed:
      FreeChunkbuffer(request.buffer);
      return false;
  }
  return false;
}

// Internal method for reading a chunk from the backing store.
bool ChunkedDevice::ReadChunk()
{
  chunk_io_request request;

  // Calculate in which chunk we are currently.
  request.chunk = current_chunk_->start_offset / current_chunk_->chunk_size;
  request.volname = current_volname_;
  request.buffer = current_chunk_->buffer;
  request.wbuflen = current_chunk_->chunk_size;
  request.rbuflen = &current_chunk_->buflen;
  request.release = false;

  current_chunk_->end_offset
      = current_chunk_->start_offset + (current_chunk_->chunk_size - 1);

  if (!ReadRemoteChunk(&request)) {
    // If the chunk doesn't exist on the backing store it has a size of 0 bytes.
    current_chunk_->buflen = 0;
    return false;
  }

  return true;
}

/*
 * Setup a chunked volume for reading or writing.
 * return:
 *  -1: failure
 *   0: success
 */
int ChunkedDevice::SetupChunk(const char*, int flags, int)
{
  int retval = -1;
  std::lock_guard<std::recursive_mutex> chunk_lock(chunk_mutex_);

  // Kept chunks get one upload try, so a recovered store ends read-only.
  if (io_threads_ == 0 && kept_count_ > 0) { TryKeptChunks(false); }

  /* If device is (re)opened and we are put into readonly mode because
   * of problems flushing chunks to the backing store we return EROFS
   * to the upper layers. */
  if ((flags & O_RDWR) && readonly_) {
    if (io_threads_ == 0 && kept_count_ > 0 && !WriterCanceled()) {
      // Fails the job once, so that it does not wait for an operator mount.
      PoolMem message(PM_MESSAGE);
      Mmsg(message, T_("Device %s refuses writes: %s\n"), print_name(),
           GetUploadState().readonly_reason.c_str());
      FailWriter(message.c_str());
    }
    dev_errno = EROFS; /** Read-only file system */
    return -1;
  }

  if (!CheckRemoteConnection()) {
    Dmsg0(100, "setup_chunk failed, as remote device is not available\n");
    dev_errno = EIO; /**< I/O error */
    return -1;
  }

  if (!current_chunk_) {
    current_chunk_ = (chunk_descriptor*)malloc(sizeof(chunk_descriptor));
    memset(current_chunk_, 0, sizeof(chunk_descriptor));
    if (chunk_size_ > DEFAULT_CHUNK_SIZE) {
      current_chunk_->chunk_size = chunk_size_;
    } else {
      current_chunk_->chunk_size = DEFAULT_CHUNK_SIZE;
    }
    current_chunk_->start_offset = -1;
    current_chunk_->end_offset = -1;
  }

  // Reopen of a device.
  if (current_chunk_->opened) {
    // Invalidate chunk.
    current_chunk_->buflen = 0;
    current_chunk_->start_offset = -1;
    current_chunk_->end_offset = -1;
  }

  if (flags & O_RDWR) { current_chunk_->writing = true; }

  current_chunk_->chunk_setup = false;

  /* We need to limit the maximum size of a chunked volume to MAX_CHUNKS *
   * chunk_size). */
  if (max_volume_size == 0
      || max_volume_size
             > (uint64_t)(MAX_CHUNKS * current_chunk_->chunk_size)) {
    max_volume_size = MAX_CHUNKS * current_chunk_->chunk_size;
  }

  // On open set begin offset to 0.
  offset_ = 0;

  // On open we are no longer at the End of the Media.
  end_of_media_ = false;

  // Keep track of the volume currently mounted.
  if (current_volname_) { free(current_volname_); }

  current_volname_ = strdup(getVolCatName());

  /* in principle it is not required to load_chunk(),
   * but we need a secure way to determine,
   * if the chunk already exists. */
  if (LoadChunk()) {
    current_chunk_->opened = true;
    retval = 0;
  } else if (flags & O_CREAT) {
    /* create a chunk */
    if (FlushChunk(false /* release */, false /* move_to_next_chunk */)) {
      current_chunk_->opened = true;
      retval = 0;
    }
  }

  return retval;
}

// Read a chunked volume.
ssize_t ChunkedDevice::ReadChunked(int, void* buffer, size_t count)
{
  ssize_t retval = 0;
  std::lock_guard<std::recursive_mutex> chunk_lock(chunk_mutex_);

  if (current_chunk_->opened) {
    ssize_t wanted_offset;
    ssize_t bytes_left;

    /* Shortcut logic see if end_of_media_ is set then we are at the End of the
     * Media */
    if (end_of_media_) { goto bail_out; }

    /* If we are starting reading without the chunk being setup it means we
     * are start reading at the beginning of the file otherwise the d_lseek
     * method would have read in the correct chunk. */
    if (!current_chunk_->chunk_setup) {
      current_chunk_->start_offset = 0;

      // See if we have to allocate a new buffer.
      if (!current_chunk_->buffer) {
        current_chunk_->buffer = allocate_chunkbuffer();
      }

      if (!ReadChunk()) {
        retval = -1;
        goto bail_out;
      }
      current_chunk_->chunk_setup = true;
    }

    // See if we can fulfill the wanted read from the current chunk.
    if (current_chunk_->start_offset <= offset_
        && current_chunk_->end_offset >= (boffset_t)((offset_ + count) - 1)) {
      wanted_offset = (offset_ % current_chunk_->chunk_size);

      bytes_left = MIN((ssize_t)count,
                       ((ssize_t)current_chunk_->buflen - wanted_offset));
      Dmsg2(200,
            "Reading complete %" PRIiz
            " byte read-request from chunk offset %" PRIiz "\n",
            bytes_left, wanted_offset);

      if (bytes_left < 0) {
        retval = -1;
        goto bail_out;
      }

      if (bytes_left > 0) {
        memcpy(buffer, current_chunk_->buffer + wanted_offset, bytes_left);
      }
      offset_ += bytes_left;
      retval = bytes_left;
      goto bail_out;
    } else {
      ssize_t offset = 0;

      /* We cannot fulfill the read from the current chunk, see how much
       * is available and return that and see if by reading the next chunk
       * we can fulfill the whole read. When then we still have not filled
       * the whole buffer we keep on reading any next chunk until none are
       * left and we have reached End Of Media. */
      while (retval < (ssize_t)count) {
        // See how much is left in this chunk.
        if (offset_ <= current_chunk_->end_offset) {
          wanted_offset = (offset_ % current_chunk_->chunk_size);
          bytes_left = MIN(((ssize_t)count - offset),
                           ((ssize_t)current_chunk_->buflen - wanted_offset));

          if (bytes_left > 0) {
            Dmsg2(
                200,
                "Reading %zd bytes of %zu byte read-request from end of chunk "
                "at offset %zd\n",
                bytes_left, count, wanted_offset);

            memcpy(((char*)buffer + offset),
                   current_chunk_->buffer + wanted_offset, bytes_left);
            offset_ += bytes_left;
            offset += bytes_left;
            retval += bytes_left;
          }
        }

        // Read in the next chunk.
        current_chunk_->start_offset += current_chunk_->chunk_size;
        if (!ReadChunk()) {
          switch (dev_errno) {
            case EIO:
              /* If the are no more chunks to read we return only the bytes
               * available. We also set end_of_media_ as we are at the end of
               * media. */
              end_of_media_ = true;
              goto bail_out;
            default:
              retval = -1;
              goto bail_out;
          }
        } else {
          /* Calculate how much data we can read from the just freshly read
           * chunk. */
          bytes_left
              = MIN(((ssize_t)count - offset), (ssize_t)current_chunk_->buflen);

          if (bytes_left > 0) {
            Dmsg2(200,
                  "Reading %" PRIiz " bytes of %" PRIuz
                  " byte read-request from next chunk\n",
                  bytes_left, count);

            memcpy(((char*)buffer + offset), current_chunk_->buffer,
                   bytes_left);
            offset_ += bytes_left;
            offset += bytes_left;
            retval += bytes_left;
          }
        }
      }
    }
  } else {
    errno = EBADF;
    retval = -1;
  }

bail_out:
  return retval;
}

// Write a chunked volume.
ssize_t ChunkedDevice::WriteChunked(int, const void* buffer, size_t count)
{
  ssize_t retval = 0;
  std::lock_guard<std::recursive_mutex> chunk_lock(chunk_mutex_);

  /* If we are put into readonly mode because of problems flushing chunks to the
   * backing store we return EIO to the upper layers. */
  if (readonly_) {
    PoolMem message(PM_MESSAGE);
    Mmsg(message, T_("Device %s refuses writes: %s\n"), print_name(),
         GetUploadState().readonly_reason.c_str());
    FailWriter(message.c_str());
    dev_errno = EIO;
    errno = EIO;
    retval = -1;
    goto bail_out;
  }

  if (current_chunk_->opened) {
    ssize_t wanted_offset;

    /* If we are starting writing without the chunk being setup it means we
     * are start writing to an empty file because otherwise the d_lseek method
     * would have read in the correct chunk. */
    if (!current_chunk_->chunk_setup) {
      current_chunk_->start_offset = 0;
      current_chunk_->end_offset = (current_chunk_->chunk_size - 1);
      current_chunk_->buflen = 0;
      current_chunk_->chunk_setup = true;

      // See if we have to allocate a new buffer.
      if (!current_chunk_->buffer) {
        current_chunk_->buffer = allocate_chunkbuffer();
      }
    }

    // See if we can write the whole data inside the current chunk.
    if (current_chunk_->start_offset <= offset_
        && current_chunk_->end_offset >= (boffset_t)((offset_ + count) - 1)) {
      wanted_offset = (offset_ % current_chunk_->chunk_size);

      Dmsg2(200,
            "Writing complete %" PRIuz
            " byte write-request to chunk offset %" PRIiz "\n",
            count, wanted_offset);

      memcpy(current_chunk_->buffer + wanted_offset, buffer, count);

      offset_ += count;
      if ((wanted_offset + count) > current_chunk_->buflen) {
        current_chunk_->buflen = wanted_offset + count;
      }
      current_chunk_->need_flushing = true;
      retval = count;
    } else {
      ssize_t bytes_left;
      ssize_t offset = 0;

      /* Things don't fit so first write as many bytes as can be written into
       * the current chunk and then flush it and write the next bytes into the
       * next chunk. When then things still don't fit loop until all bytes are
       * written. */
      while (retval < (ssize_t)count) {
        // See how much is left in this chunk.
        if (offset_ <= current_chunk_->end_offset) {
          wanted_offset = (offset_ % current_chunk_->chunk_size);
          bytes_left
              = MIN(((ssize_t)count - offset),
                    (ssize_t)((current_chunk_->end_offset
                               - (current_chunk_->start_offset + wanted_offset))
                              + 1));

          if (bytes_left > 0) {
            Dmsg2(200,
                  "Writing %" PRIiz " bytes of %" PRIuz
                  " byte write-request to end of chunk "
                  "at offset %" PRIiz "\n",
                  bytes_left, count, wanted_offset);

            memcpy(current_chunk_->buffer + wanted_offset,
                   ((char*)buffer + offset), bytes_left);
            offset_ += bytes_left;
            if ((wanted_offset + bytes_left)
                > (ssize_t)current_chunk_->buflen) {
              current_chunk_->buflen = wanted_offset + bytes_left;
            }
            current_chunk_->need_flushing = true;
            offset += bytes_left;
            retval += bytes_left;
          }
        }

        // Flush out the current chunk.
        if (!FlushChunk(true /* release */, true /* move_to_next_chunk */)) {
          dev_errno = EIO;
          errno = EIO;
          retval = -1;
          goto bail_out;
        }

        /* Calculate how much data we can fit into the just freshly created
         * chunk. */
        bytes_left = MIN(((ssize_t)count - offset),
                         (ssize_t)((current_chunk_->end_offset
                                    - current_chunk_->start_offset)
                                   + 1));
        if (bytes_left > 0) {
          Dmsg2(200,
                "Writing %" PRIiz " bytes of %" PRIuz
                " byte write-request to next chunk\n",
                bytes_left, count);

          memcpy(current_chunk_->buffer, ((char*)buffer + offset), bytes_left);
          current_chunk_->buflen = bytes_left;
          current_chunk_->need_flushing = true;
          offset_ += bytes_left;
          offset += bytes_left;
          retval += bytes_left;
        }
      }
    }
  } else {
    errno = EBADF;
    retval = -1;
  }

bail_out:
  return retval;
}

// Close a chunked volume.
int ChunkedDevice::CloseChunk()
{
  int retval = -1;
  std::lock_guard<std::recursive_mutex> chunk_lock(chunk_mutex_);

  // Kept chunks whose retry is due get one upload try.
  if (io_threads_ == 0 && kept_count_ > 0) { TryKeptChunks(true); }

  if (current_chunk_->opened) {
    if (current_chunk_->need_flushing) {
      if (FlushChunk(true /* release */, false /* move_to_next_chunk */)) {
        retval = 0;
      } else {
        dev_errno = EIO;
      }
    } else {
      /* If ChunkedDevice::wait_until_chunks_written() has been called before,
       * chunk has been flushed (buffer given to an io thread),
       * but not released. Therefore the buffer is set to NULL,
       * as normally done by flush_chunk(true, *). */
      if (io_threads_ && current_chunk_->buffer) {
        FreeChunkbuffer(current_chunk_->buffer);
        current_chunk_->buffer = NULL;
      }
      retval = 0;
    }


    // Invalidate chunk.
    current_chunk_->writing = false;
    current_chunk_->opened = false;
    current_chunk_->chunk_setup = false;
    current_chunk_->buflen = 0;
    current_chunk_->start_offset = -1;
    current_chunk_->end_offset = -1;
  } else {
    errno = EBADF;
  }

  return retval;
}

// Truncate a chunked volume.
bool ChunkedDevice::TruncateChunkedVolume(DeviceControlRecord* dcr)
{
  std::lock_guard<std::recursive_mutex> chunk_lock(chunk_mutex_);
  if (current_chunk_->opened) {
    // Chunks still waiting for upload would land in the truncated volume.
    if (const int pending = PendingChunksOfVolume(getVolCatName());
        pending > 0) {
      PoolMem message(PM_MESSAGE);
      Mmsg(message,
           T_("Volume %s on device %s has %d chunk(s) waiting for upload; it "
              "cannot be truncated before they are uploaded.\n"),
           getVolCatName(), print_name(), pending);
      PmStrcpy(errmsg, message.c_str());
      Jmsg(dcr ? dcr->jcr : nullptr, M_ERROR, 0, "%s", message.c_str());
      dev_errno = EBUSY;
      return false;
    }
    if (!TruncateRemoteVolume(dcr)) { return false; }

    // Reinitialize the initial chunk.
    current_chunk_->start_offset = 0;
    current_chunk_->end_offset = (current_chunk_->chunk_size - 1);
    current_chunk_->buflen = 0;
    current_chunk_->chunk_setup = true;
    current_chunk_->need_flushing = false;

    // Reinitialize the volume name on a relabel we could get a new name.
    if (current_volname_) { free(current_volname_); }

    current_volname_ = strdup(getVolCatName());
  }

  return true;
}

static int CompareVolumeName(void* item1, void* item2)
{
  const char* volname = (const char*)item2;
  chunk_io_request* request = (chunk_io_request*)item1;

  return strcmp(request->volname, volname);
}

// Get the current size of a volume.
ssize_t ChunkedDevice::ChunkedVolumeSize()
{
  std::lock_guard<std::recursive_mutex> chunk_lock(chunk_mutex_);

  /* Check if the current chunk needs flushing. In that case, we just wrote to
   * the chunk. As we can only ever write to the last chunk, it is safe to
   * assume that the end of the last write in this chunk is the end of the
   * volume. */
  if (current_chunk_->need_flushing) {
    return current_chunk_->start_offset + current_chunk_->buflen;
  }

  /* See if we are using io-threads or not and the ordered CircularBuffer is
   * created. We try to make sure that nothing of the volume being requested is
   * still inflight as then the RemoteVolumeSize() method will fail to
   * determine the size of the data as its not fully stored on the backing store
   * yet. */
  if (io_threads_ > 0 && cb_) {
    while (1) {
      if (!cb_->empty()) {
        chunk_io_request* request;

        /* Peek on the ordered circular queue if there are any pending
         * IO-requests for this volume. If there are use that as the indication
         * of the size of the volume and don't contact the remote storage as
         * there is still data inflight and as such we need to look at the last
         * chunk that is still not uploaded of the volume. */
        request = (chunk_io_request*)cb_->peek(
            storagedaemon::PEEK_LAST, current_volname_, CompareVolumeName);
        if (request) {
          ssize_t retval;

          // Calculate the size of the volume based on the last chunk inflight.
          retval = (request->chunk * current_chunk_->chunk_size)
                   + request->wbuflen;

          /* The peek method gives us a cloned chunk_io_request with pointers to
           * the original chunk_io_request. We just need to free the structure
           * not the content so we call free() here and not FreeChunkIoRequest()
           * ! */
          free(request);

          return retval;
        }
      }

      /* Chunk doesn't seem to be on the ordered circular buffer.
       * Make sure there is also nothing inflight to the backing store anymore.
       */
      if (NrInflightChunks() > 0) {
        uint8_t retries = INFLIGHT_RETRIES;

        /* There seem to be inflight chunks to the backing store so busy wait
         * until there is nothing inflight anymore. The chunks either get
         * uploaded and as such we can just get the volume size from the backing
         * store or it gets put back onto the ordered circular list and then we
         * can pick it up by retrying the PEEK_LAST on the ordered circular
         * list. */
        do {
          Bmicrosleep(INFLIGT_RETRY_TIME, 0);
        } while (NrInflightChunks() > 0 && --retries > 0);

        /* If we ran out of retries we most likely encountered a stale inflight
         * file. */
        if (!retries) {
          ClearInflightChunk(NULL);
          break;
        }

        /* Do a new try on the ordered circular list to get the last pending
         * IO-request for the volume we are trying to get the size of. */
        continue;
      } else {
        /* Its not on the ordered circular list and not inflight so it must be
         * on the backing store so we break the loop and try to get the volume
         * size from the chunks available on the backing store. */
        break;
      }
    }
  }

  /* Get the actual length by contacting the remote backing store. An error is
   * -1, never a size. */
  return RemoteVolumeSize().value_or(-1);
}

bool ChunkedDevice::is_written()
{
  /* See if we are using io-threads or not and the ordered circbuf is created.
   * We try to make sure that nothing of the volume being requested is still
   * inflight as then the RemoteVolumeSize() method will fail to
   * determine the size of the data as its not fully stored on the backing store
   * yet. */

  // A kept chunk of any volume is not stored yet.
  if (kept_count_ > 0) {
    Dmsg0(100, "storage is pending, as chunks are kept for upload\n");
    return false;
  }

  // A writer holding the chunk is still changing it.
  std::unique_lock<std::recursive_mutex> chunk_lock(chunk_mutex_,
                                                    std::try_to_lock);
  if (!chunk_lock.owns_lock()) {
    Dmsg0(100, "storage is pending, as a writer holds the current chunk\n");
    return false;
  }

  if (current_chunk_->need_flushing) {
    Dmsg1(100, "volume %s is pending, as current chunk needs flushing\n",
          current_volname_);
    return false;
  }

  // Make sure there is also nothing inflight to the backing store anymore.
  int inflight_chunks = NrInflightChunks();
  if (inflight_chunks > 0) {
    Dmsg2(100, "volume %s is pending, as there are %d inflight chunks\n",
          current_volname_, inflight_chunks);
    return false;
  }

  if (io_threads_ > 0 && cb_) {
    if (!cb_->empty_with_no_reserve()) {
      chunk_io_request* request;

      /* Peek on the ordered circular queue if there are any pending IO-requests
       * for this volume. If there are use that as the indication of the size of
       * the volume and don't contact the remote storage as there is still data
       * inflight and as such we need to look at the last chunk that is still
       * not uploaded of the volume. */
      request = (chunk_io_request*)cb_->peek(
          storagedaemon::PEEK_FIRST, current_volname_, CompareVolumeName);
      if (request) {
        free(request);
        Dmsg1(100, "volume %s is pending, as there are queued write requests\n",
              current_volname_);
        return false;
      }
      Dmsg0(100,
            "storage is pending, as there are queued write requests for "
            "previous volumes.\n");
      return false;
    }
  }

  /* compare expected to written volume size; an unknown size is checked again
   * later, a volume without chunks holds 0 bytes */
  const std::optional<ssize_t> remote_size = RemoteVolumeSize();
  if (!remote_size) {
    const std::string error = MessageText(errmsg);
    Dmsg2(100, "volume %s is pending, as its remote size is unknown: %s\n",
          current_volname_, error.c_str());
    SetLastError(error);
    return false;
  }
  const uint64_t remote_volume_size
      = *remote_size > 0 ? static_cast<uint64_t>(*remote_size) : 0;
  Dmsg3(100,
        "volume: %s, RemoteVolumeSize = %" PRIu64
        ", VolCatInfo.VolCatBytes "
        "= %" PRIu64 "\n",
        current_volname_, remote_volume_size, VolCatInfo.VolCatBytes);

  if (remote_volume_size < VolCatInfo.VolCatBytes) {
    Dmsg3(100,
          "volume %s is pending, as 'remote volume size' = %" PRIu64
          " < 'catalog "
          "volume size' = %" PRIu64 "\n",
          current_volname_, remote_volume_size, VolCatInfo.VolCatBytes);
    return false;
  }

  return true;
}


/* Waits until every chunk is uploaded, woken by each finished upload try;
 * without io-threads it retries the kept chunks. Fails with the reason set
 * when the job is canceled, when the device is read-only, or when no upload
 * succeeded for flush_timeout_ seconds. */
bool ChunkedDevice::WaitUntilChunksWritten(
    const std::function<bool()>& is_canceled,
    std::string& reason)
{
  using clock = FlushWaitTracker::clock;
  constexpr auto recheck_interval
      = std::chrono::seconds{DEFAULT_RECHECK_INTERVAL_WRITE_BUFFER};
  constexpr auto poll_interval = std::chrono::seconds{1};
  PoolMem message(PM_MESSAGE);
  std::string volname;
  bool current_flushed = false;
  bool not_writing = false;

  /* Hands the dirty current chunk to the uploads once no writer holds it;
   * false when the device refuses it. Runs without the device lock. */
  auto flush_current = [&]() {
    std::unique_lock<std::recursive_mutex> chunk_lock(chunk_mutex_,
                                                      std::try_to_lock);
    if (!chunk_lock.owns_lock()) { return true; }
    current_flushed = true;
    if (!current_chunk_ || !current_chunk_->writing) {
      not_writing = true;
      return true;
    }
    volname = current_volname_ ? current_volname_ : "";
    if (!current_chunk_->need_flushing) { return true; }
    if (io_threads_ == 0) {
      KeepCurrentChunk(false, {});
      return true;
    }
    std::string refusal;
    if (FlushChunkCopy(refusal)) { return true; }
    Mmsg(message, T_("Device %s refuses writes: %s\n"), print_name(),
         refusal.c_str());
    reason = message.c_str();
    dev_errno = EIO;
    return false;
  };

  if (!flush_current()) { return false; }
  // A device not opened for writing has none of its own chunks to wait for.
  if (not_writing) { return true; }

  FlushWaitTracker tracker{std::chrono::seconds{flush_timeout_}, clock::now(),
                           GetUploadState().done};
  auto next_check = clock::now();
  uint64_t seen_events = 0;
  bool written = false;

  while (true) {
    if (!current_flushed) {
      if (!flush_current()) { return false; }
      if (not_writing) { return true; }
    }
    if (io_threads_ == 0 && kept_count_ > 0) { TryKeptChunks(true); }

    if (clock::now() >= next_check) {
      // Read before the check, so that no wake-up is lost.
      seen_events = GetUploadState().events;
      written = current_flushed && is_written();
      next_check = clock::now() + recheck_interval;
    }

    // Kept chunks make the device read-only; this wait retries them.
    const bool retrying_kept = io_threads_ == 0 && kept_count_ > 0;
    const UploadState uploads = GetUploadState();
    switch (tracker.Check(written, is_canceled(), readonly_ && !retrying_kept,
                          uploads.done, clock::now())) {
      case FlushWaitResult::kWritten:
        return true;
      case FlushWaitResult::kWaiting:
        break;
      case FlushWaitResult::kCanceled:
        Mmsg(message,
             T_("Upload of volume %s on device %s not finished: job "
                "canceled.\n"),
             volname.c_str(), print_name());
        reason = message.c_str();
        dev_errno = EIO;
        return false;
      case FlushWaitResult::kReadOnly:
        Mmsg(message, T_("Device %s refuses writes: %s\n"), print_name(),
             uploads.readonly_reason.c_str());
        reason = message.c_str();
        dev_errno = EIO;
        return false;
      case FlushWaitResult::kTimedOut:
        if (retrying_kept) {
          std::lock_guard<std::mutex> lock(kept_mutex_);
          if (!kept_chunks_.empty()) {
            Mmsg(message,
                 T_("Upload of chunk %d of volume %s on device %s made no "
                    "progress for %" PRIu32
                    " seconds; the chunk stays kept and is tried again when "
                    "the device is opened or closed. Last backend error: "
                    "%s\n"),
                 kept_chunks_.front().chunk,
                 kept_chunks_.front().volname.c_str(), print_name(),
                 flush_timeout_,
                 uploads.last_error.empty() ? "none"
                                            : uploads.last_error.c_str());
            reason = message.c_str();
            dev_errno = EIO;
            return false;
          }
        }
        Mmsg(message,
             T_("Upload of volume %s on device %s made no progress for %" PRIu32
                " seconds; the pending chunks stay queued. Last backend "
                "error: %s\n"),
             volname.c_str(), print_name(), flush_timeout_,
             uploads.last_error.empty() ? "none" : uploads.last_error.c_str());
        reason = message.c_str();
        dev_errno = EIO;
        return false;
    }

    std::unique_lock<std::mutex> lock(upload_mutex_);
    if (upload_cv_.wait_for(lock, poll_interval, [this, seen_events] {
          return upload_state_.events != seen_events;
        })) {
      next_check = clock::now();
    }
  }
}


static int CloneIoRequest(void* item1, void* item2)
{
  chunk_io_request* src = (chunk_io_request*)item1;
  chunk_io_request* dst = (chunk_io_request*)item2;

  if (bstrcmp(src->volname, dst->volname) && src->chunk == dst->chunk) {
    memcpy(dst->buffer, src->buffer, src->wbuflen);
    *dst->rbuflen = src->wbuflen;

    // Cloning succeeded.
    return 0;
  }

  // Not the right volname or chunk.
  return -1;
}

// Make sure we have the right chunk in memory.
bool ChunkedDevice::LoadChunk()
{
  boffset_t start_offset;
  std::lock_guard<std::recursive_mutex> chunk_lock(chunk_mutex_);

  start_offset
      = (offset_ / current_chunk_->chunk_size) * current_chunk_->chunk_size;

  // See if we have to allocate a new buffer.
  if (!current_chunk_->buffer) {
    current_chunk_->buffer = allocate_chunkbuffer();
  }

  // If the wrong chunk is loaded populate the chunk buffer with the right data.
  if (start_offset != current_chunk_->start_offset) {
    current_chunk_->buflen = 0;
    current_chunk_->start_offset = start_offset;

    /* See if we are using io-threads or not and the ordered CircularBuffer is
     * created. We try to make sure that nothing of the volume being requested
     * is still inflight as then the ReadChunk() method will fail to read the
     * data as its not stored on the backing store yet. */
    if (io_threads_ > 0 && cb_) {
      chunk_io_request request;

      request.chunk = current_chunk_->start_offset / current_chunk_->chunk_size;
      request.volname = current_volname_;
      request.buffer = current_chunk_->buffer;
      request.rbuflen = &current_chunk_->buflen;

      while (1) {
        if (!cb_->empty()) {
          /* Peek on the ordered circular queue and clone the data which is
           * infligt back to the current chunk buffer. When we are able to clone
           * the data the peek will return the address of the request structure
           * it used for the clone operation. When nothing could be cloned it
           * will return NULL. If data is cloned we use that and skip the call
           * to read the data from the backing store as that will not have the
           * latest data anyway. */
          if (cb_->peek(storagedaemon::PEEK_CLONE, &request, CloneIoRequest)
              == &request) {
            current_chunk_->end_offset
                = start_offset + (current_chunk_->chunk_size - 1);
            goto bail_out;
          }
        }

        /* Chunk doesn't seem to be on the ordered circular buffer.
         * Make sure its also not inflight to the backing store. */
        if (IsInflightChunk(&request)) {
          uint8_t retries = INFLIGHT_RETRIES;

          /* Chunk seems to be inflight busy wait until its no longer.
           * It either gets uploaded and as such we can just read it from the
           * backing store again or it gets put back onto the ordered circular
           * list and then we can pick it up by retrying the PEEK_CLONE on the
           * ordered circular list. */
          do {
            Bmicrosleep(INFLIGT_RETRY_TIME, 0);
          } while (IsInflightChunk(&request) && --retries > 0);

          /* If we ran out of retries we most likely encountered a stale
           * inflight file. */
          if (!retries) {
            ClearInflightChunk(&request);
            break;
          }

          // Do a new try to clone the data from the ordered circular list.
          continue;
        } else {
          /* Its not on the ordered circular list and not inflight so it must be
           * on the backing store so we break the loop and try to read the chunk
           * from the backing store. */
          break;
        }
      }
    }

    // Read the chunk from the backing store.
    if (!ReadChunk()) {
      switch (dev_errno) {
        case EIO:
          if (current_chunk_->writing) {
            current_chunk_->end_offset
                = start_offset + (current_chunk_->chunk_size - 1);
          }
          return false;
          break;
        default:
          return false;
      }
    }
  }

bail_out:
  current_chunk_->chunk_setup = true;

  return true;
}

static int ListIoRequest(void* request, void* data)
{
  chunk_io_request* io_request = (chunk_io_request*)request;
  DeviceStatusInformation* dst = (DeviceStatusInformation*)data;
  PoolMem status(PM_MESSAGE);

  status.bsprintf("   /%s/%04d - %" PRIu32 " (try=%d)\n", io_request->volname,
                  io_request->chunk, io_request->wbuflen, io_request->tries);
  dst->status_length = PmStrcat(dst->status, status.c_str());

  return 0;
}

// Return specific device status information.
bool ChunkedDevice::DeviceStatus(DeviceStatusInformation* dst)
{
  bool pending = false;
  int inflight_chunks = 0;
  PoolMem inflights(PM_MESSAGE);

  dst->status_length = 0;
  if (CheckRemoteConnection()) {
    dst->status_length
        = PmStrcpy(dst->status, T_("Backend connection is working.\n"));
  } else {
    dst->status_length
        = PmStrcpy(dst->status, T_("Backend connection is not working.\n"));
  }
  /* See if we are using io-threads or not and the ordered CircularBuffer is
   * created and not empty. */
  if (io_threads_ > 0 && cb_) {
    inflight_chunks = NrInflightChunks();
    inflights.bsprintf("Inflight chunks: %d\n", inflight_chunks);
    dst->status_length = PmStrcat(dst->status, inflights.c_str());
    if (inflight_chunks > 0) { pending = true; }
    if (!cb_->empty()) {
      pending = true;
      dst->status_length
          = PmStrcat(dst->status, T_("Pending IO flush requests:\n"));

      // Peek on the ordered circular queue and list all pending requests.
      cb_->peek(storagedaemon::PEEK_LIST, dst, ListIoRequest);
    }
  }

  if (io_threads_ == 0) {
    std::lock_guard<std::mutex> lock(kept_mutex_);
    if (!kept_chunks_.empty()) {
      pending = true;
      dst->status_length
          = PmStrcat(dst->status, T_("Kept chunks waiting for upload:\n"));
      for (const auto& entry : kept_chunks_) {
        PoolMem status(PM_MESSAGE);
        status.bsprintf("   /%s/%04d - %" PRIu32 " (try=%d)\n",
                        entry.volname.c_str(), entry.chunk, entry.buflen,
                        entry.tries);
        dst->status_length = PmStrcat(dst->status, status.c_str());
      }
    }
  }

  if (!pending) {
    dst->status_length
        = PmStrcat(dst->status, T_("No pending IO flush requests.\n"));
  }

  return (dst->status_length > 0);
}

ChunkedDevice::~ChunkedDevice()
{
  if (thread_ids_) { StopThreads(); }

  if (cb_) {
    // If there is any work on the ordered circular buffer remove it.
    if (!cb_->empty()) {
      chunk_io_request* request;
      do {
        request = (chunk_io_request*)cb_->dequeue();
        if (request) {
          request->release = true;
          FreeChunkIoRequest(request);
        }
      } while (!cb_->empty());
    }

    delete cb_;
    cb_ = NULL;
  }

  // Kept chunks not uploaded by now are lost with the daemon.
  for (auto& entry : kept_chunks_) {
    Emsg3(M_ERROR, 0,
          T_("Chunk %d of volume %s on device %s was never uploaded.\n"),
          entry.chunk, entry.volname.c_str(), print_name());
    free(entry.buffer);
  }
  kept_chunks_.clear();

  if (current_chunk_) {
    if (current_chunk_->buffer) { FreeChunkbuffer(current_chunk_->buffer); }
    free(current_chunk_);
    current_chunk_ = NULL;
  }

  if (current_volname_) { free(current_volname_); }
  close(nullptr);
}

} /* namespace storagedaemon */
