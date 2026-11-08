
#include "CommonPCH.h"
#include <private/qbezier_p.h>



WordItem::WordItem(Word word, int subWordIndex, bool colorBodies)
    : word{word}, subWordIndex{subWordIndex}, colorBodies{colorBodies} {

	if (subWordIndex == -1) return;

	auto& subWord = word.wordResultInfo.subWords[subWordIndex];

	auto shape = word.bodyShape(subWord);
	auto pos = shape.pos;

	itemPath = shape.path * shape.transform;
	itemPath.translate(pos.x(), pos.y());

	if (!subWord.debugPath.isEmpty()) {
		itemPath.addPath(subWord.debugPath);
	}

	auto bbox = itemPath.boundingRect();
	itemPath.translate(-bbox.x(), -bbox.y());
	transform.translate(-bbox.x(), -bbox.y());

	//QTransform transform{ 3,0,0,3,0,0 }
	QTransform scaleTransform{ constants::SCALE_GLYPH,0,0,constants::SCALE_GLYPH,0,0 };

	itemPath = itemPath * scaleTransform;
	transform = transform * scaleTransform;
}

WordItem::~WordItem()
{
}
void WordItem::paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) {

	if (subWordIndex == -1) return;

	auto& subWord = word.wordResultInfo.subWords[subWordIndex];

	painter->drawPath(itemPath);
    const auto& shape = word.paths[subWord.paths[0]];
    if (colorBodies) {
        const QColor colors[] = {QColor("#d7e9f2"), QColor("#e1eed9"), QColor("#e9dcf1")};
        for (size_t index = 0; index < subWord.baseGlyphs.size(); ++index) {
            auto body = shape.transform.map(subWord.baseGlyphs[index].path);
            body.translate(shape.pos);
            painter->fillPath(transform.map(body), colors[index % 3]);
        }
    }
    for (const auto& kashida : subWord.kashidaPaths) {
        if (kashida.isEmpty()) continue;
        auto world = shape.transform.map(kashida);
        world.translate(shape.pos);
        painter->fillPath(transform.map(world), QColor(235, 145, 40, 170));
    }


	auto pen = QPen(Qt::cyan);
	auto brush = QBrush(Qt::white);
	pen.setCosmetic(true);
	pen.setWidthF(1);
	double radius = 3;
	double scaleRatio = 1;

	QColor blue(4, 100, 166, 50);
	QColor color2(166, 100, 166);

	if (scene()) {
		if (!scene()->views().isEmpty()) {
			QGraphicsView* view = scene()->views().first();
			double ratio = view->physicalDpiX() / 96.0;
			double newRadius = view->mapToScene(QRect(-radius * ratio, -radius * ratio, 2 * radius * ratio, 2 * radius * ratio)).boundingRect().width() / 2;
			scaleRatio = newRadius / radius;
			radius = newRadius;
		}
	}


	if (showControls) {
		for (int i = 0; i < itemPath.elementCount(); ++i) {
			const QPainterPath::Element& elm = itemPath.elementAt(i);
			switch (elm.type) {
			case QPainterPath::MoveToElement: {
				pen.setColor(color2);
				painter->setPen(pen);
				painter->setBrush(color2);
				QRectF  rect = QRectF(elm.x - radius, elm.y - radius, 2 * radius, 2 * radius);
				painter->drawEllipse(rect);
				break;
			}
			case QPainterPath::LineToElement: {
				pen.setColor(blue);
				painter->setPen(pen);
				painter->setBrush(blue);
				QRectF  rect = QRectF(elm.x - radius, elm.y - radius, 2 * radius, 2 * radius);
				painter->drawEllipse(rect);

				if (i - 1 >= 0 && itemPath.elementAt(i - 1).type == QPainterPath::MoveToElement) {


					pen.setColor(color2);

					const QPainterPath::Element& prePoint = itemPath.elementAt(i - 1);
					pen.setWidthF(3);
					painter->setPen(pen);
					QLineF line{ prePoint.x, prePoint.y, elm.x, elm.y };

					painter->drawLine(line);

					pen.setWidthF(1);

				}

				break;
			}
			case QPainterPath::CurveToElement:
			{


				if (i - 1 >= 0 && itemPath.elementAt(i - 1).type == QPainterPath::MoveToElement) {


					pen.setColor(color2);
					pen.setWidthF(3);
					painter->setPen(pen);
					painter->setBrush(color2);
					const QPainterPath::Element& prePoint = itemPath.elementAt(i - 1);

					QLineF line{ prePoint.x, prePoint.y, elm.x, elm.y };

					painter->drawLine(line);

					pen.setWidthF(1);

				}


				const QPainterPath::Element& control1 = elm;
				const QPainterPath::Element& control2 = itemPath.elementAt(i + 1);
				const QPainterPath::Element& last = itemPath.elementAt(i + 2);

				pen.setColor(Qt::cyan);
				painter->setPen(pen);
				painter->setBrush(brush);
				QRectF  rect = QRectF(control1.x - radius, control1.y - radius, 2 * radius, 2 * radius);
				painter->drawEllipse(rect);

				pen.setColor(Qt::magenta);
				painter->setPen(pen);
				rect = QRectF(control2.x - radius, control2.y - radius, 2 * radius, 2 * radius);

				painter->drawEllipse(rect);

				pen.setColor(blue);
				painter->setPen(pen);
				painter->setBrush(blue);
				rect = QRectF(last.x - radius, last.y - radius, 2 * radius, 2 * radius);
				painter->drawEllipse(rect);


				i += 2;
				break;
			}
			default:
				qFatal("MyQPdf::generatePath(), unhandled type: %d", elm.type);
			}
		}
	}


	size_t joinIndex = 0;
	for (auto& stretching : subWord.joins) {
        const auto index = joinIndex++;


		QPolygonF topPolygon{ { stretching.topLeft.point,stretching.topBaseLine.point,stretching.topRight.point} };
		QPolygonF bottomPolygon{ { stretching.bottomLeft.point,stretching.bottomBaseLine.point,stretching.bottomRight.point} };


		pen.setColor(Qt::green);
		painter->setPen(pen);


		painter->drawPolyline(topPolygon * transform);
		painter->drawPolyline(bottomPolygon * transform);

		auto pen2 = QPen(Qt::darkCyan);
		pen2.setCosmetic(true);
		pen2.setWidthF(3);
        const bool resolved = index < subWord.kashidaPaths.size() && !subWord.kashidaPaths[index].isEmpty();
        if (!resolved && stretching.counterEntry.pathIndex == 0) pen2.setStyle(Qt::DashLine);
        painter->setPen(pen2);

        if (resolved) {
            painter->drawLine(subWord.kashidaCuts[index].first * transform);
            painter->drawLine(subWord.kashidaCuts[index].second * transform);
        } else {
            for (const auto& segment : stretching.cutSegments()) painter->drawLine(segment * transform);
        }

	}

	for (auto& xextr : subWord.extrema) {

		pen.setColor(Qt::yellow);
		painter->setPen(pen);
		if (xextr.isY) {
			if (xextr.isSharp) {
				painter->setBrush(Qt::red);
			}
			else {
				painter->setBrush(Qt::darkRed);
			}

		}
		else if (xextr.isX) {

			painter->setBrush(Qt::blue);
		}
		else {

			painter->setBrush(Qt::green);
		}

		auto point = transform.map(xextr.point);

		QRectF  rect = QRectF(point.x() - radius, point.y() - radius, 2 * radius, 2 * radius);
		painter->drawEllipse(rect);

		QString pos = QString("%1-%2")
			.arg((int)xextr.point.x())
			.arg((int)xextr.point.y());

		pos = QString("%1").arg(xextr.time);

		pen.setColor(Qt::black);
		painter->setPen(pen);
		QFont font = painter->font();		
		font.setPointSizeF(6 * scaleRatio);
		painter->setFont(font);
		painter->drawText(point.x() + 3 * scaleRatio, point.y() - 3 * scaleRatio, pos);
		

	}


}
