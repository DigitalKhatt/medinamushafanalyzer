#include "CustomOutputDev.h";
#include "PDFDoc.h";
#include <iostream>
#include <QPainterPath>
#include "afont.h"


static QPainterPath convertPath(GfxState* state, const GfxPath* path, Qt::FillRule fillRule)
{
	int i, j;

	QPainterPath qPath;
	qPath.setFillRule(fillRule);
	for (i = 0; i < path->getNumSubpaths(); ++i) {
		const GfxSubpath* subpath = path->getSubpath(i);
		if (subpath->getNumPoints() > 0) {
			qPath.moveTo(subpath->getX(0), subpath->getY(0));
			j = 1;
			while (j < subpath->getNumPoints()) {
				if (subpath->getCurve(j)) {
					qPath.cubicTo(subpath->getX(j), subpath->getY(j), subpath->getX(j + 1), subpath->getY(j + 1), subpath->getX(j + 2), subpath->getY(j + 2));
					j += 3;
				}
				else {
					qPath.lineTo(subpath->getX(j), subpath->getY(j));
					++j;
				}
			}
			if (subpath->isClosed()) {
				qPath.closeSubpath();
			}
		}
	}
	return qPath;
}

static std::vector<APathItem> convertPaths(GfxState* state, const GfxPath* path, Qt::FillRule fillRule, QBrush m_currentBrush)
{
	int i, j;

	std::vector<APathItem> paths;
	auto ctm = state->getCTM();
	auto transform = QTransform(ctm[0], ctm[1], ctm[2], ctm[3], 0, 0);

	for (i = 0; i < path->getNumSubpaths(); ++i) {
		const GfxSubpath* subpath = path->getSubpath(i);
		if (subpath->getNumPoints() > 0) {
			QPainterPath qPath;
			qPath.setFillRule(fillRule);

			qPath.moveTo(subpath->getX(0), subpath->getY(0));
			j = 1;
			while (j < subpath->getNumPoints()) {
				if (subpath->getCurve(j)) {
					qPath.cubicTo(subpath->getX(j), subpath->getY(j), subpath->getX(j + 1), subpath->getY(j + 1), subpath->getX(j + 2), subpath->getY(j + 2));
					j += 3;
				}
				else {
					qPath.lineTo(subpath->getX(j), subpath->getY(j));
					++j;
				}
			}
			if (subpath->isClosed()) {
				qPath.closeSubpath();
			}
			bool add = true;
			// TODO : Group paths of the same glyph such as two dots , fathatan, three dots, dammatan,  ...
			// this avoids that some paths belong to the wrong line and allows to coicide the number of paths with the calculated one for each line 
			if (!paths.empty()) {
				auto& back = paths.back();
				auto backPath = back.path;
				if (backPath.contains(qPath)) {
					backPath.addPath(qPath);
					back.path = backPath;
					add = false;
				}
				else if (qPath.contains(backPath)) {
					qPath.addPath(backPath);
					back.path = qPath;
					add = false;
				}
				/*
				else if (AFont::scaleAndCompareQPainterPath(qPath, backPath) < constants::MATCHING_ERROR_RATIO) {
					int tt = 5;

				}*/
			}
			if (add) {

				APathItem item{ qPath  ,m_currentBrush,transform, QPointF{ctm[4], ctm[5]} };
				/*auto item = new QGraphicsPathItem(qPath);
				item->setFlag(QGraphicsItem::ItemIsSelectable, true);
				item->setTransform(transform);
				item->setBrush(m_currentBrush);
				item->setPen(Qt::NoPen);
				item->setPos(ctm[4], ctm[5]);*/
				paths.push_back(item);
			}

		}
	}
	return paths;
}

CustomOutputDev::CustomOutputDev() {
	m_currentBrush = QBrush(Qt::SolidPattern);
}
void CustomOutputDev::startDoc(PDFDoc* docA)
{
	doc = docA;
	// std::cout << "startDoc" << std::endl;
}

CustomOutputDev::~CustomOutputDev()
{
}

void CustomOutputDev::startPage(int pageNum, GfxState* state, XRef*) {
	// std::cout << "startPage" << std::endl;
}

void CustomOutputDev::endPage() {
	// std::cout << "endPage" << std::endl;
}

void CustomOutputDev::saveState(GfxState* state)
{
	//std::cout << "saveState" << std::endl;
	m_currentBrushStack.push(m_currentBrush);
}
void CustomOutputDev::restoreState(GfxState* state)
{
	//std::cout << "restoreState" << std::endl;
	m_currentBrush = m_currentBrushStack.top();
	m_currentBrushStack.pop();
}
void CustomOutputDev::updateAll(GfxState* state)
{
	OutputDev::updateAll(state);
	//std::cout << "updateAll" << std::endl;
}
void CustomOutputDev::setDefaultCTM(const double* ctm)
{
	//std::cout << "setDefaultCTM " << ctm[0] << " " << ctm[1] << " " << ctm[2] << " " << ctm[3] << " " << ctm[4] << " " << ctm[5] << std::endl;

}
void CustomOutputDev::updateCTM(GfxState* state, double m11, double m12, double m21, double m22, double m31, double m32)
{
	//std::cout << "updateCTM " << m11 << " " << m12 << " " << m21 << " " << m22 << " " << m31 << " " << m32 << std::endl;
}
void CustomOutputDev::stroke(GfxState* state) {
	//std::cout << "stroke" << std::endl;
}
/*
void CustomOutputDev::fill(GfxState* state) {
	std::cout << "fill" << std::endl;
	auto ctm = state->getCTM();
	auto transform = QTransform(ctm[0], ctm[1], ctm[2], ctm[3], 0, 0);
	auto item = new QGraphicsPathItem(convertPath(state, state->getPath(), Qt::WindingFill) );
	item->setTransform(transform);
	item->setBrush(m_currentBrush);
	item->setPen(Qt::NoPen);
	item->setPos(ctm[4], ctm[5]);
	paths.push_back(item);
}
void CustomOutputDev::eoFill(GfxState* state) {
	std::cout << "eoFill" << std::endl;
	auto ctm = state->getCTM();
	auto transform = QTransform(ctm[0], ctm[1], ctm[2], ctm[3], 0, 0);
	auto path = convertPath(state, state->getPath(), Qt::OddEvenFill);
	//path = path * transform;
	auto item = new QGraphicsPathItem(path);
	item->setTransform(transform);
	item->setBrush(m_currentBrush);
	item->setPen(Qt::NoPen);
	item->setPos(ctm[4], ctm[5]);
	paths.push_back(item);
}*/
void CustomOutputDev::fill(GfxState* state) {

	auto tt = convertPaths(state, state->getPath(), Qt::WindingFill, m_currentBrush);
	paths.insert(
		paths.end(),
		std::make_move_iterator(tt.begin()),
		std::make_move_iterator(tt.end())
	);
}
void CustomOutputDev::eoFill(GfxState* state) {
	auto tt = convertPaths(state, state->getPath(), Qt::OddEvenFill, m_currentBrush);
	paths.insert(
		paths.end(),
		std::make_move_iterator(tt.begin()),
		std::make_move_iterator(tt.end())
	);

}
bool CustomOutputDev::axialShadedFill(GfxState* state, GfxAxialShading* shading, double tMin, double tMax) {
	//std::cout << "axialShadedFill" << std::endl;
	return false;
}

//----- path clipping
void CustomOutputDev::clip(GfxState* state) {
	//std::cout << "clip" << std::endl;
}
void CustomOutputDev::eoClip(GfxState* state) {
	//std::cout << "eoClip" << std::endl;
}
void CustomOutputDev::clipToStrokePath(GfxState* state) {
	//std::cout << "clipToStrokePath" << std::endl;
}
void CustomOutputDev::updateFillColor(GfxState* state)
{
	GfxRGB rgb;
	QColor brushColour = m_currentBrush.color();
	state->getFillRGB(&rgb);
	brushColour.setRgbF(colToDbl(rgb.r), colToDbl(rgb.g), colToDbl(rgb.b), brushColour.alphaF());
	m_currentBrush.setColor(brushColour);
}
void CustomOutputDev::updateFillOpacity(GfxState* state)
{
	QColor brushColour = m_currentBrush.color();
	brushColour.setAlphaF(state->getFillOpacity());
	m_currentBrush.setColor(brushColour);
}


