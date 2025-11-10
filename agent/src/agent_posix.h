// Copyright 2022 The Chromium Authors.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Modified copy of content_analysis_sdk/agent/src/agent_posix.h
// for use in ORTSOC DLP agent.

#ifndef CONTENT_ANALYSIS_SRC_AGENT_POSIX_H_
#define CONTENT_ANALYSIS_SRC_AGENT_POSIX_H_

#include <map>
#include <string>
#include "agent_base.h"
#include "content_analysis/sdk/analysis.pb.h"

namespace content_analysis {
namespace sdk {

// Agent implementaton for linux.
class AgentPosix : public AgentBase {
 public:
  AgentPosix(Config config, std::unique_ptr<AgentEventHandler> handler);
  ~AgentPosix();

  ResultCode HandleEvents() override;
  ResultCode Stop() override;
  std::string DebugString() const override;
  
  private:
  // Send response back to client
  bool SendResponse(int client_fd, const AgentToChrome& response);

  friend std::unique_ptr<Agent> Agent::Create(Config, std::unique_ptr<AgentEventHandler>, ResultCode*);
  ResultCode Initialize();
  void HandleNewConnection();
  bool HandleClientMessage(int client_fd);
  void RemoveClient(int client_fd);
  void Cleanup();

  int server_socket_;
  std::string socket_path_;
  std::map<int, BrowserInfo> clients_;  // client_fd -> BrowserInfo
  bool stop_requested_;
};

}  // namespace sdk
}  // namespace content_analysis

#endif  // CONTENT_ANALYSIS_SRC_AGENT_POSIX_H_