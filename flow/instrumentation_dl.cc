// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flow/instrumentation_dl.h"
#include "display_list/dl_paint.h"
#include "flow/instrumentation.h"
#include "third_party/skia/include/core/SkPath.h"

namespace flutter {

static const double kOneFrameMS = 1e3 / 60.0;
static const size_t kMaxSamples = 120;
static const size_t kMaxFrameMarkers = 8;

DlStopwatch::DlStopwatch(const Stopwatch::RefreshRateUpdater& updater)
    : Stopwatch(updater) {}

void DlStopwatch::Visualize(DlCanvas* canvas, const SkRect& rect) const {
  DlPaint paint;

  // Paint the background.
  paint.setColor(0x99FFFFFF);
  canvas->DrawRect(rect, paint);

  // Establish the graph position.
  const SkScalar x = rect.x();
  const SkScalar y = rect.y();
  const SkScalar width = rect.width();
  const SkScalar height = rect.height();
  const SkScalar bottom = y + height;
  const SkScalar right = x + width;

  // Scale the graph to show frame times up to those that are 3 times the frame
  // time.
  const double max_interval = kOneFrameMS * 3.0;
  const double max_unit_interval = UnitFrameInterval(max_interval);

  // Prepare a path for the data.
  // We start at the height of the last point, so it looks like we wrap around.
  SkPath path;
  path.setIsVolatile(true);
  const double sample_unit_width = (1.0 / kMaxSamples);
  path.moveTo(x, bottom);
  path.lineTo(x, y + height * (1.0 - UnitHeight(laps_[0].ToMillisecondsF(),
                                                max_unit_interval)));
  double unit_x;
  double unit_next_x = 0.0;
  for (size_t i = 0; i < kMaxSamples; i += 1) {
    unit_x = unit_next_x;
    unit_next_x = (static_cast<double>(i + 1) / kMaxSamples);
    const double sample_y =
        y + height * (1.0 - UnitHeight(laps_[i].ToMillisecondsF(),
                                       max_unit_interval));
    path.lineTo(x + width * unit_x, sample_y);
    path.lineTo(x + width * unit_next_x, sample_y);
  }
  path.lineTo(
      right,
      y + height * (1.0 - UnitHeight(laps_[kMaxSamples - 1].ToMillisecondsF(),
                                     max_unit_interval)));
  path.lineTo(right, bottom);
  path.close();

  // Draw the graph.
  paint.setColor(0xAA0000FF);
  canvas->DrawPath(path, paint);

  // Draw horizontal markers.
  paint.setStrokeWidth(0);  // hairline
  paint.setDrawStyle(DlDrawStyle::kStroke);
  paint.setColor(0xCC000000);

  if (max_interval > kOneFrameMS) {
    // Paint the horizontal markers
    size_t frame_marker_count = static_cast<size_t>(max_interval / kOneFrameMS);

    // Limit the number of markers displayed. After a certain point, the graph
    // becomes crowded
    if (frame_marker_count > kMaxFrameMarkers) {
      frame_marker_count = 1;
    }

    for (size_t frame_index = 0; frame_index < frame_marker_count;
         frame_index++) {
      const double frame_height =
          height * (1.0 - (UnitFrameInterval((frame_index + 1) * kOneFrameMS) /
                           max_unit_interval));

      auto start = SkPoint::Make(x, y + frame_height);
      auto end = SkPoint::Make(right, y + frame_height);
      canvas->DrawLine(start, end, paint);
    }
  }

  // Paint the vertical marker for the current frame.
  // We paint it over the current frame, not after it, because when we
  // paint this we don't yet have all the times for the current frame.
  paint.setDrawStyle(DlDrawStyle::kStroke);
  if (UnitFrameInterval(LastLap().ToMillisecondsF()) > 1.0) {
    // budget exceeded
    paint.setColor(SK_ColorRED);
  } else {
    // within budget
    paint.setColor(SK_ColorGREEN);
  }
  double sample_x =
      x + width * (static_cast<double>(current_sample_) / kMaxSamples);

  const auto marker_rect = SkRect::MakeLTRB(
      sample_x, y, sample_x + width * sample_unit_width, bottom);
  canvas->DrawRect(marker_rect, paint);
}

double Stopwatch::UnitFrameInterval(double raster_time_ms) const {
  return raster_time_ms / GetFrameBudget().count();
}

double Stopwatch::UnitHeight(double raster_time_ms,
                             double max_unit_interval) const {
  double unit_height = UnitFrameInterval(raster_time_ms) / max_unit_interval;
  if (unit_height > 1.0) {
    unit_height = 1.0;
  }
  return unit_height;
}

}  // namespace flutter
