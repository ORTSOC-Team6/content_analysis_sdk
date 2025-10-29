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
#include <climits>

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
  std::cout << "[Agent] Using socket path: " << path << std::endl;
  return path;
}

bool WriteMessage(int socket_fd, const std::string& message) {
  if (message.empty()) {
    std::cerr << "[Agent] Cannot write empty message" << std::endl;
    return false;
  }

  std::cout << "[Agent] Writing message to fd=" << socket_fd << ", size=" << message.size() << std::endl;

  // Write message length first (4 bytes)
  uint32_t length = static_cast<uint32_t>(message.size());
  ssize_t written = write(socket_fd, &length, sizeof(length));
  if (written != sizeof(length)) {
    std::cerr << "[Agent] Failed to write message length: " << strerror(errno) << std::endl;
    return false;
  }

  // Write message data
  written = write(socket_fd, message.data(), message.size());
  if (written != static_cast<ssize_t>(message.size())) {
    std::cerr << "[Agent] Failed to write message data: " << strerror(errno) << std::endl;
    return false;
  }

  std::cout << "[Agent] Successfully wrote message" << std::endl;
  return true;
}

bool ReadMessage(int socket_fd, std::string* message) {
  // Read message length first (4 bytes)
  uint32_t length;
  ssize_t bytes_read = read(socket_fd, &length, sizeof(length));
  if (bytes_read != sizeof(length)) {
    if (bytes_read == 0) {
      std::cout << "[Agent] Client disconnected (EOF)" << std::endl;
    } else {
      std::cerr << "[Agent] Failed to read message length from fd=" << socket_fd << ": " << strerror(errno) << std::endl;
    }
    return false;
  }

  std::cout << "[Agent] Reading message from fd=" << socket_fd << ", expected size=" << length << std::endl;

  // Validate message length
  if (length > 1024 * 1024) {  // 1MB limit
    std::cerr << "[Agent] Message too large: " << length << " bytes" << std::endl;
    return false;
  }

  // Read message data
  std::vector<char> buffer(length);
  bytes_read = read(socket_fd, buffer.data(), length);
  if (bytes_read != static_cast<ssize_t>(length)) {
    std::cerr << "[Agent] Failed to read message data: expected " << length << ", got " << bytes_read << std::endl;
    return false;
  }

  message->assign(buffer.data(), length);
  std::cout << "[Agent] Successfully read message" << std::endl;
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
  std::cout << "[Agent] Initializing POSIX agent..." << std::endl;
  
  server_socket_ = socket(AF_UNIX, SOCK_STREAM, 0);
  if (server_socket_ == -1) {
    std::cerr << "[Agent] Failed to create socket: " << strerror(errno) << std::endl;
    return ResultCode::ERR_UNEXPECTED;
  }

  std::cout << "[Agent] Created socket with fd=" << server_socket_ << std::endl;

  // Set socket to non-blocking for better event handling
  int flags = fcntl(server_socket_, F_GETFL, 0);
  if (flags == -1 || fcntl(server_socket_, F_SETFL, flags | O_NONBLOCK) == -1) {
    std::cerr << "[Agent] Failed to set socket non-blocking: " << strerror(errno) << std::endl;
    close(server_socket_);
    server_socket_ = -1;
    return ResultCode::ERR_UNEXPECTED;
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
  std::cout << "[Agent] Cleaned up any existing socket file" << std::endl;

  if (bind(server_socket_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == -1) {
    std::cerr << "[Agent] Failed to bind to " << socket_path_ << ": " << strerror(errno) << std::endl;
    close(server_socket_);
    server_socket_ = -1;
    return ResultCode::ERR_UNEXPECTED;
  }

  std::cout << "[Agent] Successfully bound to " << socket_path_ << std::endl;

  if (listen(server_socket_, 5) == -1) {
    std::cerr << "[Agent] Failed to listen on socket: " << strerror(errno) << std::endl;
    close(server_socket_);
    server_socket_ = -1;
    return ResultCode::ERR_UNEXPECTED;
  }

  std::cout << "[Agent] Listening for connections (backlog=5)" << std::endl;
  std::cout << "[Agent] Agent ready - Firefox should connect to: " << socket_path_ << std::endl;
  
  // Show socket permissions for debugging
  std::string ls_cmd = "ls -la " + socket_path_;
  std::cout << "[Agent] Socket permissions: ";
  std::cout.flush();
  system(ls_cmd.c_str());

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
    std::cerr << "Accept failed: " << strerror(errno) << std::endl;
    return;  // Accept failed, continue
  }

  std::cout << "New client connected: fd=" << client_fd << std::endl;

  // Keep client socket in BLOCKING mode for easier message handling
  // Don't set O_NONBLOCK flag on client sockets

  // Get actual client process info using SO_PEERCRED
  BrowserInfo browser_info;
  struct ucred cred;
  socklen_t len = sizeof(cred);
  
  if (getsockopt(client_fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0) {
    browser_info.pid = cred.pid;
    
    // Try to get the process executable path
    std::string proc_path = "/proc/" + std::to_string(cred.pid) + "/exe";
    char exe_path[PATH_MAX];
    ssize_t path_len = readlink(proc_path.c_str(), exe_path, sizeof(exe_path) - 1);
    if (path_len != -1) {
      exe_path[path_len] = '\0';
      browser_info.binary_path = exe_path;
    } else {
      browser_info.binary_path = "unknown";
    }
    
    std::cout << "Client info: pid=" << browser_info.pid << " path=" << browser_info.binary_path << std::endl;
  } else {
    // Fallback if SO_PEERCRED fails
    std::cerr << "Failed to get client credentials: " << strerror(errno) << std::endl;
    browser_info.pid = 0;
    browser_info.binary_path = "unknown";
  }

  clients_[client_fd] = browser_info;
  
  if (handler()) {
    handler()->OnBrowserConnected(browser_info);
  }
}

bool AgentPosix::HandleClientMessage(int client_fd) {
  std::string message;
  if (!ReadMessage(client_fd, &message)) {
    std::cout << "Failed to read message from client fd=" << client_fd << std::endl;
    return false;  // Read failed, client should be removed
  }

  std::cout << "Received message from client fd=" << client_fd << ", size=" << message.size() << std::endl;

  ChromeToAgent chrome_to_agent;
  if (!chrome_to_agent.ParseFromString(message)) {
    std::cout << "Failed to parse protobuf message from client fd=" << client_fd << std::endl;
    return false;  // Invalid message format
  }

  auto client_it = clients_.find(client_fd);
  if (client_it == clients_.end()) {
    std::cout << "Client fd=" << client_fd << " not found in client list" << std::endl;
    return false;  // Client not found
  }

  const BrowserInfo& browser_info = client_it->second;

  if (chrome_to_agent.has_request()) {
    // Handle content analysis request
    auto event = std::make_unique<ContentAnalysisEventPosix>(
        client_fd, browser_info, std::move(*chrome_to_agent.mutable_request()));
    
    // Initialize the event (this sets up the default response)
    ResultCode rc = event->Init();
    if (rc != ResultCode::OK) {
      std::cerr << "Failed to initialize event: " << static_cast<int>(rc) << std::endl;
      return false;  // Event initialization failed
    }
    
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
  std::cout << "Removing client fd=" << client_fd << std::endl;
  
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

}  // namespace sdk
}  // namespace content_analysis
