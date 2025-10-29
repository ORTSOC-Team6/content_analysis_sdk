// Copyright 2022 The Chromium Authors.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Modified copy of content_analysis_sdk/agent/src/event_posix.cc
// for use in ORTSOC DLP agent.

#include "event_posix.h"
#include "scoped_print_handle_posix.h"
#include "content_analysis/sdk/analysis.pb.h"
#include <unistd.h>
#include <sstream>

namespace content_analysis {
namespace sdk {

namespace {

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

}  // namespace

ContentAnalysisEventPosix::ContentAnalysisEventPosix(
    int client_fd,
    const BrowserInfo& browser_info,
    ContentAnalysisRequest req)
    : ContentAnalysisEventBase(browser_info), client_fd_(client_fd) {
  *request() = std::move(req);
}

ResultCode ContentAnalysisEventPosix::Send() {
  AgentToChrome agent_to_chrome;
  *agent_to_chrome.mutable_response() = *response();

  std::string serialized = agent_to_chrome.SerializeAsString();
  if (WriteMessage(client_fd_, serialized)) {
    return ResultCode::OK;
  } else {
    return ResultCode::ERR_UNEXPECTED;
  }
}

std::string ContentAnalysisEventPosix::DebugString() const {
  std::stringstream state;
  state << "ContentAnalysisEventPosix{client_fd=" << client_fd_;
  state << " request_token=" << GetRequest().request_token();
  state << "}";
  return state.str();
}

}  // namespace sdk
}  // namespace content_analysis
