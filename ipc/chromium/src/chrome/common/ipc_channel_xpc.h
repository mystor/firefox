/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
// Copyright (c) 2008 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_COMMON_IPC_CHANNEL_XPC_H_
#define CHROME_COMMON_IPC_CHANNEL_XPC_H_

#include "chrome/common/ipc_channel.h"

#include "base/process.h"

#include "mozilla/EventTargetAndLockCapability.h"
#include "mozilla/UniquePtr.h"
#include "nsISupports.h"

namespace IPC {

// An implementation of ChannelImpl for POSIX systems that works via
// socketpairs.  See the .cc file for an overview of the implementation.
class Channel::ChannelImpl final {
 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING_WITH_DELETE_ON_EVENT_TARGET(
      ChannelImpl, IOThread().GetEventTarget());

  // Mirror methods of Channel, see ipc_channel.h for description.
  ChannelImpl(ChannelHandle pipe, Mode mode, base::ProcessId other_pid);
  bool Connect(Listener* listener) MOZ_EXCLUDES(SendMutex());
  void Close() MOZ_EXCLUDES(SendMutex());

  // NOTE: `Send` may be called on threads other than the I/O thread.
  bool Send(mozilla::UniquePtr<Message> message) MOZ_EXCLUDES(SendMutex());

  void SetOtherPid(base::ProcessId other_pid);

  // See the comment in ipc_channel.h for info on IsClosed()
  // NOTE: `IsClosed` may be called on threads other than the I/O thread.
  bool IsClosed() MOZ_EXCLUDES(SendMutex()) {
    mozilla::MutexAutoLock lock(SendMutex());
    chan_cap_.NoteLockHeld();
    return IsClosedCap();
  }

 private:
  ~ChannelImpl() { Close(); }

  bool IsClosedCap() MOZ_REQUIRES_SHARED(chan_cap_) {
    return !server_conn_ && !conn_;
  }

  void Init(Mode mode) MOZ_REQUIRES(SendMutex(), IOThread());
  bool EnqueueHelloMessage() MOZ_REQUIRES(SendMutex(), IOThread());
  void CloseLocked() MOZ_REQUIRES(SendMutex(), IOThread());
  void SendLocked(mozilla::UniquePtr<Message> msg) MOZ_REQUIRES(SendMutex());

  bool ContinueConnect(xpc_object_t event) MOZ_REQUIRES(IOThread());
  bool ProcessIncomingMessages(xpc_object_t event) MOZ_REQUIRES(IOThread());

  const mozilla::EventTargetCapability<nsISerialEventTarget>& IOThread() const
      MOZ_RETURN_CAPABILITY(chan_cap_.Target()) {
    return chan_cap_.Target();
  }

  mozilla::Mutex& SendMutex() MOZ_RETURN_CAPABILITY(chan_cap_.Lock()) {
    return chan_cap_.Lock();
  }

  // Compound capability of the IO thread and a Mutex.
  mozilla::EventTargetAndLockCapability<nsISerialEventTarget, mozilla::Mutex>
      chan_cap_;

  Mode mode_ MOZ_GUARDED_BY(IOThread());

  // Only initialized in server mode. We appear to need to keep a reference to
  // the server connection around, as destroying it also shuts down conn_.
  mozilla::DarwinObjectPtr<xpc_connection_t> server_conn_
      MOZ_GUARDED_BY(chan_cap_);
  mozilla::DarwinObjectPtr<xpc_connection_t> conn_ MOZ_GUARDED_BY(chan_cap_);

  Listener* listener_ MOZ_GUARDED_BY(IOThread()) = nullptr;

  // Messages to be sent are queued here until the connection is ready.
  nsTArray<mozilla::UniquePtr<Message>> output_queue_
      MOZ_GUARDED_BY(SendMutex());

  // We keep track of the PID of the other side of this channel so that we can
  // record this when generating logs of IPC messages.
  base::ProcessId other_pid_ MOZ_GUARDED_BY(chan_cap_) =
      base::kInvalidProcessId;

  DISALLOW_COPY_AND_ASSIGN(ChannelImpl);
};

}  // namespace IPC

#endif  // CHROME_COMMON_IPC_CHANNEL_XPC_H_
