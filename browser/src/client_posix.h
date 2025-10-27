// Copyright 2022 The Chromium Authors.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Modified copy of content_analysis_sdk/browser/src/client_posix.h
// for use in ORTSOC DLP agent.

#ifndef CONTENT_ANALYSIS_BROWSER_SRC_CLIENT_POSIX_H_
#define CONTENT_ANALYSIS_BROWSER_SRC_CLIENT_POSIX_H_

#include "client_base.h"

namespace content_analysis {
namespace sdk {

// Client implementaton for Posix.
class ClientPosix : public ClientBase {
 public:
  ClientPosix(Config config);
  ~ClientPosix();

  // Client:
  int Send(ContentAnalysisRequest request,
           ContentAnalysisResponse* response) override;
  int Acknowledge(const ContentAnalysisAcknowledgement& ack) override;
  int CancelRequests(const ContentAnalysisCancelRequests& cancel) override;

  bool IsConnected() const;

 private:
  bool Connect();
  void Disconnect();

  int socket_fd_;
};

}  // namespace sdk
}  // namespace content_analysis

#endif  // CONTENT_ANALYSIS_BROWSER_SRC_CLIENT_POSIX_H_