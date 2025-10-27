// Copyright 2022 The Chromium Authors.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Modified copy of content_analysis_sdk/browser/src/client_posix.cc
// for use in ORTSOC DLP agent.

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <cstring>
#include <vector>
#include <utility>

#include "client_posix.h"
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
std::unique_ptr<Client> Client::Create(Config config) {
  auto client = std::make_unique<ClientPosix>(std::move(config));
  if (client->IsConnected()) {
    return std::move(client);
  }
  return nullptr;
}

ClientPosix::ClientPosix(Config config) : ClientBase(std::move(config)), socket_fd_(-1) {
  Connect();
}

ClientPosix::~ClientPosix() {
  Disconnect();
}

bool ClientPosix::Connect() {
  if (socket_fd_ != -1) {
    return true;  // Already connected
  }

  socket_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
  if (socket_fd_ == -1) {
    return false;
  }

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;

  std::string socket_path = GetSocketPath(configuration().name, configuration().user_specific);
  if (socket_path.size() >= sizeof(addr.sun_path)) {
    close(socket_fd_);
    socket_fd_ = -1;
    return false;
  }

  strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

  if (connect(socket_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == -1) {
    close(socket_fd_);
    socket_fd_ = -1;
    return false;
  }

  // Try to get agent info (best effort)
  agent_info().pid = 0;  // TODO: Could implement getting agent PID if needed
  agent_info().binary_path = "";

  return true;
}

void ClientPosix::Disconnect() {
  if (socket_fd_ != -1) {
    close(socket_fd_);
    socket_fd_ = -1;
  }
}

bool ClientPosix::IsConnected() const {
  return socket_fd_ != -1;
}

int ClientPosix::Send(ContentAnalysisRequest request,
                      ContentAnalysisResponse* response) {
  if (!IsConnected()) {
    return -1;
  }

  ChromeToAgent chrome_to_agent;
  *chrome_to_agent.mutable_request() = std::move(request);

  std::string serialized = chrome_to_agent.SerializeAsString();
  if (!WriteMessage(socket_fd_, serialized)) {
    return -1;
  }

  std::string response_data;
  if (!ReadMessage(socket_fd_, &response_data)) {
    return -1;
  }

  AgentToChrome agent_to_chrome;
  if (!agent_to_chrome.ParseFromString(response_data)) {
    return -1;
  }

  *response = std::move(*agent_to_chrome.mutable_response());
  return 0;
}

int ClientPosix::Acknowledge(const ContentAnalysisAcknowledgement& ack) {
  if (!IsConnected()) {
    return -1;
  }

  ChromeToAgent chrome_to_agent;
  *chrome_to_agent.mutable_ack() = ack;

  std::string serialized = chrome_to_agent.SerializeAsString();
  return WriteMessage(socket_fd_, serialized) ? 0 : -1;
}

int ClientPosix::CancelRequests(const ContentAnalysisCancelRequests& cancel) {
  if (!IsConnected()) {
    return -1;
  }

  ChromeToAgent chrome_to_agent;
  *chrome_to_agent.mutable_cancel() = cancel;

  std::string serialized = chrome_to_agent.SerializeAsString();
  return WriteMessage(socket_fd_, serialized) ? 0 : -1;
}

}  // namespace sdk
}  // namespace content_analysis
