#pragma once

#include "qpainterpath.h"
#include "qbrush.h"
#include "qtransform.h"
#include "qpoint.h"

namespace constants
{
	inline extern int constexpr SCALE_GLYPH = (4800/72);
	inline extern double constexpr MATCHING_ERROR_RATIO = 0.1;

} // namespace constants

struct APathItem {
	QPainterPath path;
	QBrush brush;
	QTransform transform;
	QPointF pos;
};
