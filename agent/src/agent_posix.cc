// Copyright 2022 The Chromium Authors.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Modified copy of content_analysis_sdk/agent/src/agent_posix.cc
// for use in ORTSOC DLP agent.

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <cstring>
#include <vector>
#include <utility>
#include <iostream>
#include <sstream>

#include "agent_posix.h"
#include "event_posix.h"
#include "content_analysis/sdk/analysis.pb.h"

namespace content_analysis {
namespace sdk {

namespace {

std::string GetSocketPath(const std::string& name, bool user_specific) {
  std::string path;
  if (user_specific) {
    path = "/tmp/content_analysis_" + name + "_" + std::to_string(getuid());
  } else {
    path = "/tmp/content_analysis_" + name;
  }
  return path;
}

bool WriteMessage(int socket_fd, const std::string& message) {
  if (message.empty()) {
    return false;
  }

  // Write message length first (4 bytes)
  uint32_t length = static_cast<uint32_t>(message.size());
  ssize_t written = write(socket_fd, &length, sizeof(length));
  if (written != sizeof(length)) {
    return false;
  }

  // Write message data
  written = write(socket_fd, message.data(), message.size());
  return written == static_cast<ssize_t>(message.size());
}

bool ReadMessage(int socket_fd, std::string* message) {
  // Read message length first (4 bytes)
  uint32_t length;
  ssize_t bytes_read = read(socket_fd, &length, sizeof(length));
  if (bytes_read != sizeof(length)) {
    return false;
  }

  // Read message data
  std::vector<char> buffer(length);
  bytes_read = read(socket_fd, buffer.data(), length);
  if (bytes_read != static_cast<ssize_t>(length)) {
    return false;
  }

  message->assign(buffer.data(), length);
  return true;
}

}  // namespace

// static
std::unique_ptr<Agent> Agent::Create(
    Config config,
    std::unique_ptr<AgentEventHandler> handler,
    ResultCode* rc) {
  auto agent = std::make_unique<AgentPosix>(std::move(config), std::move(handler));
  *rc = agent->Initialize();
  if (*rc != ResultCode::OK) {
    return nullptr;
  }
  return std::move(agent);
}

AgentPosix::AgentPosix(
    Config config,
    std::unique_ptr<AgentEventHandler> handler)
  : AgentBase(std::move(config), std::move(handler)),
    server_socket_(-1),
    stop_requested_(false) {
  
  // Block SIGPIPE to avoid crashes when clients disconnect unexpectedly
  signal(SIGPIPE, SIG_IGN);
}

AgentPosix::~AgentPosix() {
  Stop();
  Cleanup();
}

ResultCode AgentPosix::Initialize() {
  server_socket_ = socket(AF_UNIX, SOCK_STREAM, 0);
  if (server_socket_ == -1) {
    return ResultCode::ERR_CANNOT_CREATE_CHANNEL;
  }

  // Set socket to non-blocking for better event handling
  int flags = fcntl(server_socket_, F_GETFL, 0);
  if (flags == -1 || fcntl(server_socket_, F_SETFL, flags | O_NONBLOCK) == -1) {
    close(server_socket_);
    server_socket_ = -1;
    return ResultCode::ERR_CANNOT_CREATE_CHANNEL;
  }

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;

  socket_path_ = GetSocketPath(configuration().name, configuration().user_specific);
  if (socket_path_.size() >= sizeof(addr.sun_path)) {
    close(server_socket_);
    server_socket_ = -1;
    return ResultCode::ERR_INVALID_CHANNEL_NAME;
  }

  strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

  // Remove existing socket file if it exists
  unlink(socket_path_.c_str());

  if (bind(server_socket_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == -1) {
    close(server_socket_);
    server_socket_ = -1;
    return ResultCode::ERR_CANNOT_CREATE_CHANNEL;
  }

  if (listen(server_socket_, 5) == -1) {
    close(server_socket_);
    server_socket_ = -1;
    return ResultCode::ERR_CANNOT_CREATE_CHANNEL;
  }

  return ResultCode::OK;
}

ResultCode AgentPosix::HandleEvents() {
  std::vector<struct pollfd> poll_fds;
  
  while (!stop_requested_) {
    // Prepare poll file descriptors
    poll_fds.clear();
    
    // Add server socket
    poll_fds.push_back({server_socket_, POLLIN, 0});
    
    // Add client sockets
    for (const auto& client : clients_) {
      poll_fds.push_back({client.first, POLLIN, 0});
    }

    // Poll with timeout
    int result = poll(poll_fds.data(), poll_fds.size(), 1000);  // 1 second timeout
    
    if (result == -1) {
      if (errno == EINTR) {
        continue;  // Interrupted by signal, continue
      }
      return ResultCode::ERR_UNEXPECTED;
    }
    
    if (result == 0) {
      continue;  // Timeout, check stop condition
    }

    // Handle server socket events (new connections)
    if (poll_fds[0].revents & POLLIN) {
      HandleNewConnection();
    }

    // Handle client socket events
    for (size_t i = 1; i < poll_fds.size(); ++i) {
      if (poll_fds[i].revents & (POLLIN | POLLHUP | POLLERR)) {
        int client_fd = poll_fds[i].fd;
        if (poll_fds[i].revents & POLLIN) {
          if (!HandleClientMessage(client_fd)) {
            RemoveClient(client_fd);
          }
        } else {
          // Client disconnected or error
          RemoveClient(client_fd);
        }
      }
    }
  }

  return ResultCode::OK;
}

ResultCode AgentPosix::Stop() {
  stop_requested_ = true;
  return ResultCode::OK;
}

std::string AgentPosix::DebugString() const {
  std::stringstream state;
  state << "AgentPosix{socket_path=\"" << socket_path_;
  state << "\" server_socket=" << server_socket_;
  state << " clients=" << clients_.size();
  state << " stop_requested=" << stop_requested_;
  state << "}";
  return state.str();
}

void AgentPosix::HandleNewConnection() {
  struct sockaddr_un client_addr;
  socklen_t client_len = sizeof(client_addr);
  
  int client_fd = accept(server_socket_, 
                        reinterpret_cast<struct sockaddr*>(&client_addr),
                        &client_len);
  
  if (client_fd == -1) {
    return;  // Accept failed, continue
  }

  // Set client socket to non-blocking
  int flags = fcntl(client_fd, F_GETFL, 0);
  if (flags != -1) {
    fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);
  }

  // Create browser info for this client
  BrowserInfo browser_info;
  browser_info.pid = 0;  // TODO: Could implement getting client PID if needed
  browser_info.binary_path = "";

  clients_[client_fd] = browser_info;
  
  if (handler()) {
    handler()->OnBrowserConnected(browser_info);
  }
}

bool AgentPosix::HandleClientMessage(int client_fd) {
  std::string message;
  if (!ReadMessage(client_fd, &message)) {
    return false;  // Read failed, client should be removed
  }

  ChromeToAgent chrome_to_agent;
  if (!chrome_to_agent.ParseFromString(message)) {
    return false;  // Invalid message format
  }

  auto client_it = clients_.find(client_fd);
  if (client_it == clients_.end()) {
    return false;  // Client not found
  }

  const BrowserInfo& browser_info = client_it->second;

  if (chrome_to_agent.has_request()) {
    // Handle content analysis request
    auto event = std::make_unique<ContentAnalysisEventPosix>(
        client_fd, browser_info, std::move(*chrome_to_agent.mutable_request()));
    
    if (handler()) {
      handler()->OnAnalysisRequested(std::move(event));
    }
  } else if (chrome_to_agent.has_ack()) {
    // Handle acknowledgement
    if (handler()) {
      handler()->OnResponseAcknowledged(chrome_to_agent.ack());
    }
  } else if (chrome_to_agent.has_cancel()) {
    // Handle cancel request
    if (handler()) {
      handler()->OnCancelRequests(chrome_to_agent.cancel());
    }
  }

  return true;
}

void AgentPosix::RemoveClient(int client_fd) {
  auto client_it = clients_.find(client_fd);
  if (client_it != clients_.end()) {
    if (handler()) {
      handler()->OnBrowserDisconnected(client_it->second);
    }
    clients_.erase(client_it);
  }
  
  close(client_fd);
}

void AgentPosix::Cleanup() {
  // Close all client connections
  for (const auto& client : clients_) {
    close(client.first);
  }
  clients_.clear();

  // Close server socket
  if (server_socket_ != -1) {
    close(server_socket_);
    server_socket_ = -1;
  }

  // Remove socket file
  if (!socket_path_.empty()) {
    unlink(socket_path_.c_str());
    socket_path_.clear();
  }
}

bool AgentPosix::SendResponse(int client_fd, const AgentToChrome& response) {
  std::string serialized = response.SerializeAsString();
  return WriteMessage(client_fd, serialized);
}
