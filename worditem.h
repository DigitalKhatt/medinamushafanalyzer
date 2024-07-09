#pragma once

#include "qgraphicsitem.h"
#include <set>
#include <vector>
#include "pageanalysisresult.h"
#include "qpaintdevice.h"




class WordItem : public QGraphicsItem {

public:

	explicit WordItem(Word word, int subWordIndex);
	~WordItem();

	QRectF boundingRect() const override {
		auto brec = itemPath.boundingRect();
		return QRectF(brec.x() - 50, brec.y() - 50, brec.width() + 100, brec.height() + 100);
	};

	void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget = nullptr) override;

private:
	Word word;
	int subWordIndex;
	QPainterPath itemPath;
	QTransform	transform;
	bool showControls = false;

};