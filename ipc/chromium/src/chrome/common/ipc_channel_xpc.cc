/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
// Copyright (c) 2008 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/common/ipc_channel_xpc.h"

#include "mozilla/Mutex.h"

#include <xpc/xpc.h>
#include "mozilla/Mutex.h"
#include <stddef.h>

#include "base/process_util.h"
#include "chrome/common/ipc_channel_utils.h"
#include "mozilla/ipc/ProtocolUtils.h"

using namespace mozilla;
using namespace mozilla::ipc;

namespace IPC {

static constexpr const char* kFdsKey = "f";
static constexpr const char* kPortsKey = "p";
static constexpr const char* kObjectsKey = "o";
static constexpr const char* kDataKey = "d";

Channel::ChannelImpl::ChannelImpl(ChannelHandle pipe, Mode mode,
                                  base::ProcessId other_pid)
    : chan_cap_("ChannelImpl::SendMutex",
                MessageLoopForIO::current()->SerialEventTarget()),
      mode_(mode),
      other_pid_(other_pid) {
  xpc_type_t pipe_type = xpc_get_type(pipe.get());
  if (mode_ == MODE_SERVER) {
    MOZ_RELEASE_ASSERT(pipe_type == XPC_TYPE_CONNECTION,
                       "invalid server pipe argument type");
    server_conn_ = static_cast<xpc_connection_t>(pipe.get());
  } else {
    MOZ_RELEASE_ASSERT(pipe_type == XPC_TYPE_ENDPOINT,
                       "invalid client pipe argument type");
    conn_ = mozilla::AdoptDarwinObject(xpc_connection_create_from_endpoint(
        static_cast<xpc_endpoint_t>(pipe.get())));
  }

  EnqueueHelloMessage();
}

bool Channel::ChannelImpl::EnqueueHelloMessage() {
  mozilla::UniquePtr<Message> msg(
      new Message(MSG_ROUTING_NONE, HELLO_MESSAGE_TYPE));
  if (!msg->WriteInt(base::GetCurrentProcId())) {
    CloseLocked();
    return false;
  }

  SendLocked(std::move(msg));
  return true;
}

bool Channel::ChannelImpl::Connect(Listener* listener) {
  IOThread().AssertOnCurrentThread();
  chan_cap_.NoteOnTarget();

  if (IsClosedCap()) {
    return false;
  }

  listener_ = listener;

  // If we only have a server connection, attach to that connection instead.
  if (server_conn_ && !conn_) {
    RefPtr<ChannelImpl> self = this;
    xpc_connection_set_event_handler(server_conn_.get(), ^(xpc_object_t event) {
      self->IOThread().Dispatch(
          NS_NewRunnableFunction("IPC::Channel xpc init event",
                                 [self, event = DarwinObjectPtr{event}] {
                                   self->IOThread().AssertOnCurrentThread();
                                   self->chan_cap_.NoteOnTarget();

                                   // Only attempt to continue if we're still
                                   // awaiting conn_. All other messages on
                                   // server_conn_ will be ignored.
                                   if (self->server_conn_ && !self->conn_ &&
                                       !self->ContinueConnect(event.get())) {
                                     self->Close();
                                     self->listener_->OnChannelError();
                                   }
                                 }));
    });
    xpc_connection_activate(server_conn_.get());
    return true;
  }

  // The connection is immediately ready
  return ContinueConnect(conn_.get());
}

bool Channel::ChannelImpl::ContinueConnect(xpc_object_t event) {
  MOZ_ASSERT(event);

  // Validate that the event is of the correct type.
  const xpc_type_t event_type = xpc_get_type(event);
  if (event_type == XPC_TYPE_ERROR) {
    const char* description =
        xpc_dictionary_get_string(event, XPC_ERROR_KEY_DESCRIPTION);
    CHROMIUM_LOG(ERROR) << "xpc connection error: " << description;
    return false;
  }
  if (event_type != XPC_TYPE_CONNECTION) {
    CHROMIUM_LOG(ERROR) << "expected connection type";
    return false;
  }

  MutexAutoLock lock(SendMutex());
  chan_cap_.NoteExclusiveAccess();

  MOZ_ASSERT(!conn_ || conn_.get() == event);
  conn_ = static_cast<xpc_connection_t>(event);

  RefPtr<ChannelImpl> self = this;
  xpc_connection_set_event_handler(conn_.get(), ^(xpc_object_t event) {
    // For legacy reasons we need to handle the event on the I/O thread
    // (even we don't do any I/O there), so dispatch the handler there.
    self->IOThread().Dispatch(NS_NewRunnableFunction(
        "IPC::Channel xpc event", [self, event = DarwinObjectPtr{event}] {
          self->IOThread().AssertOnCurrentThread();
          self->chan_cap_.NoteOnTarget();

          if (self->conn_ && !self->ProcessIncomingMessages(event.get())) {
            self->Close();
            self->listener_->OnChannelError();
          }
        }));
  });
  xpc_connection_activate(conn_.get());

  // Flush all queued messages. The queue will not be used again.
  nsTArray<mozilla::UniquePtr<Message>> to_send = std::move(output_queue_);
  for (auto& message : to_send) {
    SendLocked(std::move(message));
  }

  return true;
}

void Channel::ChannelImpl::SetOtherPid(base::ProcessId other_pid) {
  IOThread().AssertOnCurrentThread();
  mozilla::MutexAutoLock lock(SendMutex());
  chan_cap_.NoteExclusiveAccess();
  MOZ_RELEASE_ASSERT(
      other_pid_ == base::kInvalidProcessId || other_pid_ == other_pid,
      "Multiple sources of SetOtherPid disagree!");
  other_pid_ = other_pid;
}

bool Channel::ChannelImpl::Send(mozilla::UniquePtr<Message> message) {
  // NOTE: This method may be called on threads other than `IOThread()`.
  mozilla::MutexAutoLock lock(SendMutex());
  chan_cap_.NoteLockHeld();

#ifdef IPC_MESSAGE_DEBUG_EXTRA
  DLOG(INFO) << "sending message @" << message.get() << " on channel @" << this
             << " with type " << message->type() << " ("
             << output_queue_.Length() << " in queue)";
#endif

  // If the channel has been closed, ProcessOutgoingMessages() is never going
  // to pop anything off output_queue; output_queue will only get emptied when
  // the channel is destructed.  We might as well delete message now, instead
  // of waiting for the channel to be destructed.
  if (IsClosedCap()) {
    if (mozilla::ipc::LoggingEnabled()) {
      fprintf(stderr,
              "Can't send message %s, because this channel is closed.\n",
              message->name());
    }
    return false;
  }

  SendLocked(std::move(message));
  return true;
}

void Channel::ChannelImpl::SendLocked(mozilla::UniquePtr<Message> msg) {
  chan_cap_.NoteLockHeld();

  mozilla::LogIPCMessage::LogDispatchWithPid(msg.get(), other_pid_);

  MOZ_DIAGNOSTIC_ASSERT(!IsClosedCap());
  msg->AssertAsLargeAsHeader();

  // If we're still awaiting a connection, queue up this message to be sent when
  // the connection is ready.
  // Once the connection is ready, we never end up using the queue again, as
  // xpc's send method does queueing internally.
  if (!conn_) {
    output_queue_.AppendElement(std::move(msg));
    return;
  }

  DarwinObjectPtr<xpc_object_t> xpc_msg =
      mozilla::AdoptDarwinObject(xpc_dictionary_create_empty());

  // Attach any FDs
  if (msg->num_handles() > 0) {
    DarwinObjectPtr<xpc_object_t> handles =
        AdoptDarwinObject(xpc_array_create_empty());
    for (auto& handle : msg->attached_handles_) {
      xpc_array_set_fd(handles.get(), XPC_ARRAY_APPEND, handle.get());
    }
    xpc_dictionary_set_value(xpc_msg.get(), kFdsKey, handles.get());
  }

  // Attach any send rights.
  if (msg->num_send_rights() > 0) {
    DarwinObjectPtr<xpc_object_t> send_rights =
        AdoptDarwinObject(xpc_array_create_empty());
    for (auto& send_right : msg->attached_send_rights_) {
      DarwinObjectPtr<xpc_object_t> wrapper =
          AdoptDarwinObject(xpc_dictionary_create_empty());
      xpc_dictionary_set_mach_send(wrapper.get(), kPortsKey, send_right.get());
      xpc_array_append_value(send_rights.get(), wrapper.get());
    }
    xpc_dictionary_set_value(xpc_msg.get(), kPortsKey, send_rights.get());
  }

  // Attach any loose XPC objects.
  if (msg->num_xpc_objects() > 0) {
    DarwinObjectPtr<xpc_object_t> objects =
        AdoptDarwinObject(xpc_array_create_empty());
    for (auto& object : msg->attached_xpc_objects_) {
      xpc_array_set_value(objects.get(), XPC_ARRAY_APPEND, object.get());
    }
    xpc_dictionary_set_value(xpc_msg.get(), kObjectsKey, objects.get());
  }

  // Build a contiguous buffer with the full data of the message.
  size_t size = msg->size();
  UniqueFreePtr<char> buffer(static_cast<char*>(moz_xmalloc(size)));
  const auto& msg_buffers = msg->Buffers();
  auto buff_iter = msg_buffers.Iter();
  msg_buffers.ReadBytes(buff_iter, buffer.get(), size);

  // Transfer the ownership of the buffer to xpc.
  DarwinObjectPtr<dispatch_data_t> dispatch_buf =
      AdoptDarwinObject(dispatch_data_create(buffer.release(), size, nullptr,
                                             DISPATCH_DATA_DESTRUCTOR_FREE));
  DarwinObjectPtr<xpc_object_t> xpc_data =
      xpc_data_create_with_dispatch_data(dispatch_buf.get());
  xpc_dictionary_set_value(xpc_msg.get(), kDataKey, xpc_data.get());

  // Immediately send the message over XPC.
  xpc_connection_send_message(conn_.get(), xpc_msg.get());
}

bool Channel::ChannelImpl::ProcessIncomingMessages(xpc_object_t event) {
  IOThread().AssertOnCurrentThread();
  chan_cap_.NoteOnTarget();
  MOZ_DIAGNOSTIC_ASSERT(conn_);

  // Check the message type to see if this is an error. If it is, return `false`
  // which will cause our caller to close the channel and report an error.
  xpc_type_t event_type = xpc_get_type(event);
  if (event_type == XPC_TYPE_ERROR) {
    const char* description =
        xpc_dictionary_get_string(event, XPC_ERROR_KEY_DESCRIPTION);
    CHROMIUM_LOG(ERROR) << "xpc connection error: " << description;
    return false;
  }
  if (event_type != XPC_TYPE_DICTIONARY) {
    CHROMIUM_LOG(ERROR) << "unexpected event type from xpc connection";
    return false;
  }

  // Read the message payload, and use it to construct the message.
  size_t length = 0;
  const void* buffer = xpc_dictionary_get_data(event, kDataKey, &length);
  if (!buffer) {
    CHROMIUM_LOG(ERROR) << "message payload missing";
    return false;
  }
  UniquePtr<Message> message =
      MakeUnique<Message>(static_cast<const char*>(buffer), length);

  // Attach any handles on the message.
  if (xpc_object_t fds_array = xpc_dictionary_get_array(event, kFdsKey)) {
    size_t fd_count = xpc_array_get_count(fds_array);
    if (fd_count == 0) {
      CHROMIUM_LOG(ERROR) << "unexpected empty fds array";
      return false;
    }

    nsTArray<UniqueFileHandle> handles(fd_count);
    for (size_t i = 0; i < fd_count; ++i) {
      handles.EmplaceBack(xpc_array_dup_fd(fds_array, i));
    }
    message->SetAttachedFileHandles(std::move(handles));
  }

  // Attach any send rights.
  if (xpc_object_t ports_array = xpc_dictionary_get_array(event, kPortsKey)) {
    size_t port_count = xpc_array_get_count(ports_array);
    if (port_count == 0) {
      CHROMIUM_LOG(ERROR) << "unexpected empty send rights array";
      return false;
    }

    nsTArray<UniqueMachSendRight> send_rights(port_count);
    for (size_t i = 0; i < port_count; ++i) {
      if (xpc_object_t wrapper = xpc_array_get_dictionary(ports_array, i)) {
        send_rights.EmplaceBack(
            xpc_dictionary_copy_mach_send(wrapper, kPortsKey));
      } else {
        CHROMIUM_LOG(ERROR) << "missing send right in array";
        send_rights.AppendElement();
      }
    }
    message->attached_send_rights_ = std::move(send_rights);
  }

  // Attach any loose xpc objects.
  if (xpc_object_t objects_array =
          xpc_dictionary_get_array(event, kObjectsKey)) {
    size_t objects_count = xpc_array_get_count(objects_array);
    if (objects_count == 0) {
      CHROMIUM_LOG(ERROR) << "unexpected empty objects array";
      return false;
    }

    nsTArray<DarwinObjectPtr<xpc_object_t>> xpc_objects(objects_count);
    for (size_t i = 0; i < objects_count; ++i) {
      xpc_objects.AppendElement(xpc_array_get_value(objects_array, i));
    }
    message->attached_xpc_objects_ = std::move(xpc_objects);
  }

  // Note: We set other_pid_ below when we receive a Hello message (which
  // has no routing ID), but we only emit a profiler marker for messages
  // with a routing ID, so there's no conflict here.
  AddIPCProfilerMarker(*message, other_pid_, MessageDirection::eReceiving,
                       MessagePhase::TransferEnd);

#ifdef IPC_MESSAGE_DEBUG_EXTRA
  DLOG(INFO) << "received message on channel @" << this << " with type "
             << m.type();
#endif

  if (message->routing_id() == MSG_ROUTING_NONE &&
      message->type() == HELLO_MESSAGE_TYPE) {
    // The Hello message contains only the process id.
    int32_t other_pid = MessageIterator(*message).NextInt();
    SetOtherPid(other_pid);
    listener_->OnChannelConnected(other_pid);
  } else {
    mozilla::LogIPCMessage::Run run(message.get());
    listener_->OnMessageReceived(std::move(message));
  }
  return true;
}

void Channel::ChannelImpl::Close() {
  IOThread().AssertOnCurrentThread();
  mozilla::MutexAutoLock lock(SendMutex());
  CloseLocked();
}

void Channel::ChannelImpl::CloseLocked() {
  chan_cap_.NoteExclusiveAccess();

  // Close can be called multiple times, so we need to make sure we're
  // idempotent.

  if (conn_) {
    xpc_connection_cancel(conn_.get());
    conn_ = nullptr;
  }
  if (server_conn_) {
    xpc_connection_cancel(server_conn_.get());
    server_conn_ = nullptr;
  }
}

//------------------------------------------------------------------------------
// Channel's methods simply call through to ChannelImpl.
Channel::Channel(ChannelHandle pipe, Mode mode, base::ProcessId other_pid)
    : channel_impl_(new ChannelImpl(std::move(pipe), mode, other_pid)) {
  MOZ_COUNT_CTOR(IPC::Channel);
}

Channel::~Channel() { MOZ_COUNT_DTOR(IPC::Channel); }

bool Channel::Connect(Listener* listener) {
  return channel_impl_->Connect(listener);
}

void Channel::Close() { channel_impl_->Close(); }

bool Channel::Send(mozilla::UniquePtr<Message> message) {
  return channel_impl_->Send(std::move(message));
}

void Channel::SetOtherPid(base::ProcessId other_pid) {
  channel_impl_->SetOtherPid(other_pid);
}

bool Channel::IsClosed() const { return channel_impl_->IsClosed(); }

// static
bool Channel::CreateRawPipe(ChannelHandle* server, ChannelHandle* client) {
  // Create an anonymous XPC connection, and an endpoint for that connection.
  DarwinObjectPtr<xpc_connection_t> server_conn =
      AdoptDarwinObject(xpc_connection_create(nullptr, nullptr));
  if (!server_conn) {
    return false;
  }
  DarwinObjectPtr<xpc_endpoint_t> client_endpoint =
      AdoptDarwinObject(xpc_endpoint_create(server_conn.get()));
  if (!client_endpoint) {
    return false;
  }

  // Copy the endpoint handles into the caller, erasing the specific types.
  *server = server_conn.get();
  *client = client_endpoint.get();
  return true;
}

}  // namespace IPC
