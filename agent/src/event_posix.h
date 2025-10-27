// Copyright 2022 The Chromium Authors.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Modified copy of content_analysis_sdk/agent/src/event_posix.h
// for use in ORTSOC DLP agent.

#ifndef CONTENT_ANALYSIS_SRC_EVENT_POSIX_H_
#define CONTENT_ANALYSIS_SRC_EVENT_POSIX_H_

#include "event_base.h"

namespace content_analysis {
namespace sdk {

// ContentAnalysisEvent implementaton for linux.
class ContentAnalysisEventPosix : public ContentAnalysisEventBase {
 public:
   ContentAnalysisEventPosix(int client_fd,
                             const BrowserInfo& browser_info,
                             ContentAnalysisRequest request);

  // ContentAnalysisEvent:
  ResultCode Send() override;
  std::string DebugString() const override;

 private:
  int client_fd_;
};

}  // namespace sdk
}  // namespace content_analysis

#endif  // CONTENT_ANALYSIS_SRC_EVENT_POSIX_H_