#pragma once

#include "qgraphicsitem.h"
#include <set>
#include <vector>
#include "pageanalysisresult.h"
#include "qpaintdevice.h"

struct PathExtrema {
	int pathIndex;
	qreal time;
	QPointF point;
	bool isX;
	bool isY;
	bool isSharp = false;
};

struct Stretching {
	double length;
	PathExtrema topLeft;
	PathExtrema topBaseLine;	
	PathExtrema topRight;
	PathExtrema bottomRight;
	PathExtrema bottomBaseLine;	
	PathExtrema bottomLeft;
};

auto cmp = [](Stretching a, Stretching b) { return a.length > b.length; };

using Stretchings = std::set < Stretching, decltype(cmp) > ;


class ShapeItem : public QGraphicsPathItem {

public:

	

	explicit ShapeItem(QGraphicsItem* parent = nullptr);
	explicit ShapeItem(const QPainterPath& path, QGraphicsItem* parent = nullptr);
	~ShapeItem();

	void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget = nullptr) override;
	static QVector<PathExtrema> getExtrema(const QPainterPath& path, std::vector<QPainterPath>& subPaths);
	static void searchStretchings(const QPainterPath& path, Stretchings& stretchings, ALine& line, double scaled = 1.0);
private:
	QVector<PathExtrema> extrema;
	Stretchings stretchings;
	std::vector<QPainterPath> subPaths;

};