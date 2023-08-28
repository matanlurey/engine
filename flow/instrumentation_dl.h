// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_FLOW_INSTRUMENTATION_DL_H_
#define FLUTTER_FLOW_INSTRUMENTATION_DL_H_

#include "flow/instrumentation.h"

namespace flutter {

//------------------------------------------------------------------------------
/// @brief      An implementation of |Stopwatch| that uses display list.
///
/// @note       The default implementation of |Stopwatch| uses Skia to draw
///             visualizations. Due to lax testing (for example, no tests for
///             instrumentation.cc at all), and the fact that Skia is still the
///             primary backend for non-iOS platforms, we'll keep that code
///             untouched for now.
///
///             Hypothetically, this should be backend agnostic and work with
///             any display list backend (including Skia and Impeller).
class DlStopwatch : public Stopwatch {
 public:
  explicit DlStopwatch(const RefreshRateUpdater& updater);

  virtual void Visualize(DlCanvas* canvas, const SkRect& rect) const override;
};

}  // namespace flutter

#endif  // FLUTTER_FLOW_INSTRUMENTATION_DL_H_
