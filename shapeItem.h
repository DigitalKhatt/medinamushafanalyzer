#pragma once

#include "qgraphicsitem.h"

#include "pageanalysisresult.h"
#include "qpaintdevice.h"




class ShapeItem : public QGraphicsPathItem {

public:



	explicit ShapeItem(QGraphicsItem* parent = nullptr);
	explicit ShapeItem(const QPainterPath& path, QGraphicsItem* parent = nullptr);
	~ShapeItem();

	void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget = nullptr) override;
	static QVector<PathExtrema> getExtrema(const QPainterPath& path, std::vector<QPainterPath>& subPaths, QVector<PathExtrema>& extrema);
	static void searchStretchings(const QPainterPath& path, Stretchings& stretchings, double scaled = 1.0);
	
private:
	QVector<PathExtrema> extrema;
	Stretchings stretchings;


};