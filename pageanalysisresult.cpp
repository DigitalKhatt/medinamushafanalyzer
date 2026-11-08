#include "pageanalysisresult.h"
#include "pathmask.h"
#include "CustomOutputDev.h"
#include "PDFDoc.h"
#include "GlobalParams.h"
#include "afont.h"
#include "quran.h"
#include <format>
#include "qfile.h"
#include <climits>


QMap<int, QMap<int, QMap<int, ShapeExceptionRecord>>> PageAnalysisResult::nbShapeExceptions
{

	{189, { {15, {{1,{ShapeException::SeenMediHehMediSeparated,3}}}} }},
	{235, { {3, {{8,{ShapeException::LamMediAlefFinaSeparated,2}}}} }},
	{252, { {5, {{7,{ShapeException::LamMediAlefFinaSeparated,2}}}} }},
	{259, { {10, {{-1,{ShapeException::LamMediAlefFinaSeparated,2}}}} }},
	{287, { {8, {{5,{ShapeException::LamMediAlefFinaSeparated,2}}}} }},
	{288, { {14, {{2,{ShapeException::LamMediAlefFinaSeparated,2}}}} }},
	{324,
		{
			{4, {{7,{ShapeException::AlefHamzaJoined,1}}}},
			{9, {{6,{ShapeException::LamMediAlefFinaSeparated,2}}}}
		}
	},
	{327, { {15, {{5,{ShapeException::SeenMediHehMediSeparated,2}}}} }},
	{328, { {11, {{3,{ShapeException::SeenMediHehMediSeparated,2}}}} }},
	{360,
		{
			{6, {{3,{ShapeException::LamMediAlefFinaSeparated,3}}}},
			{11, {{5,{ShapeException::LamMediAlefFinaSeparated,2}}}},
			{12, {{6,{ShapeException::LamMediAlefFinaSeparated,1}}}}
		}
	},
	{393, { {2, {{4,{ShapeException::LamMediAlefFinaSeparated,2}}}} }},
	{437, { {1, {{7,{ShapeException::InnerContourCollidingProblem,2}}}} }},
	{493, { {6, {{2,{ShapeException::LamMediAlefFinaSeparated,2}}}} }},
	{532, { {7, {{8,{ShapeException::LamMediAlefFinaSeparated,2}}}} }},

	{101, { {7, {{3,{ShapeException::LamMediAlefFinaSeparated,1}}}} }},
	{119, { {11, {{1,{ShapeException::LamMediAlefFinaSeparated,1}}}} }},
	{131, { {15, {{5,{ShapeException::LamMediAlefFinaSeparated, 1}}}} }},
	{133, { {7, {{1,{ShapeException::LamMediAlefFinaSeparated,1}}}} }},
	{138, { {4, {{5,{ShapeException::LamMediAlefFinaSeparated,1}}}} }},
	{178, { {13, {{3,{ShapeException::SeenMediHehMediSeparated,1}}}} }},
	{181, { {3, {{8,{ShapeException::SeenMediHehMediSeparated,1}}}} }},
	{203, { {7, {{-1,{ShapeException::LamMediHahMediSeparated,1}}}} }},
	{413, { {7, {{8,{ShapeException::LamMediAlefFinaSeparated,1}}}} }},
	{561, { {7, {{-1,{ShapeException::LamMediAlefFinaSeparated,1}}}} }},
	{415, { {13, {{2,{ShapeException::LamMediAlefFinaSeparated,1}}}} }},
	{341, { {7, {{3,{ShapeException::AlefHamzaJoined,1}}}} }},
	{563, { {13, {{2,{ShapeException::LamMediAlefFinaSeparated,1}}}} }},
	{417, { {4, {{5,{ShapeException::LamMediAlefFinaSeparated,1}}}} }},
	{234, { {1, {{1,{ShapeException::LamMediAlefFinaSeparated,1}}}} }},
	{200, { {6, {{6,{ShapeException::LamMediAlefFinaSeparated,1}}}} }},
	{275, { {6, {{8,{ShapeException::LamMediAlefFinaSeparated,1}}}} }},
	{423, { {2, {{-1,{ShapeException::LamMediAlefFinaSeparated,1}}}} }},
	{424, { {8, {{4,{ShapeException::LamMediAlefFinaSeparated,1}}}} }},
	{387, { {3, {{4,{ShapeException::LamMediAlefFinaSeparated,1}}}} }},
	{241, { {7, {{1,{ShapeException::LamMediAlefFinaSeparated,1}}}} }},
	{536, { {14, {{8,{ShapeException::LamMediAlefFinaSeparated,1}}}} }},
	{426, { {13, {{8,{ShapeException::LamMediAlefFinaSeparated,1}}}} }},
	{574, { {5, {{1,{ShapeException::LamMediAlefFinaSeparated,1}}}} }},
	{243, { {15, {{5,{ShapeException::LamMediAlefFinaSeparated,1}}}} }},
	{320, { {5, {{8,{ShapeException::LamMediAlefFinaSeparated,1}}}} }},
	{326, { {4, {{4,{ShapeException::LamMediAlefFinaSeparated,1}}}} }},
	{546, { {11, {{2,{ShapeException::LamMediAlefFinaSeparated,1}}}} }},
	{362,
		{
			{6, {{2,{ShapeException::LamMediAlefFinaSeparated,1}}}},
			{11, {{3,{ShapeException::LamMediAlefFinaSeparated,1}}}}
		}
	},
	{289, { {8, {{6,{ShapeException::SeenMediHehMediSeparated,1}}}} }},
	{401, { {3, {{1,{ShapeException::SeenMediHehMediSeparated,1}}}} }},
	{219, { {6, {{5,{ShapeException::NoonMediJeemMediSeparated,1}}}} }},

};

QVector<QStringList> PageAnalysisResult::quranText{};

QVector<QVector<QStringList>> PageAnalysisResult::QuranTextByWord{};

void PageAnalysisResult::initQuranText()
{
	for (int i = 0; i < 604; i++) {
		auto pageText = QString::fromUtf8(qurantext[i] + 1);
		pageText = pageText.replace(QRegularExpression(" *" + QString("۞") + " *"), QString("۞") + " ");

		auto textLines = pageText.split(char(10), Qt::SkipEmptyParts);

		if (i == 576) {
			auto line3 = textLines[2];
			textLines[2] = line3.chopped(5);
			textLines[3] = line3.right(4) + " " + textLines[3];
		}
		else if (i == 307) {
			int lineIndex = 0;
			int nbChar = 4;
			auto line = textLines[lineIndex];
			textLines[lineIndex] = line.chopped(nbChar);
			textLines[lineIndex + 1] = line.right(nbChar - 1) + " " + textLines[lineIndex + 1];
		}
		else if (i == 456) {
			int lineIndex = 10;
			int nbChar = 9;
			auto line = textLines[lineIndex];
			textLines[lineIndex] = line.chopped(nbChar);
			textLines[lineIndex + 1] = line.right(nbChar - 1) + " " + textLines[lineIndex + 1];

			lineIndex = 11;
			nbChar = 12;
			line = textLines[lineIndex];
			textLines[lineIndex] = line.chopped(nbChar);
			textLines[lineIndex + 1] = line.right(nbChar - 1) + " " + textLines[lineIndex + 1];
		}
		else if (i == 75) {
			int lineIndex = 8;
			int nbChar = 6;
			auto line = textLines[lineIndex + 1];
			textLines[lineIndex + 1] = line.mid(nbChar);
			textLines[lineIndex] = textLines[lineIndex] + " " + line.left(nbChar - 1);
		}
		else if (i == 91) {
			int lineIndex = 11;
			int nbChar = 4;
			auto line = textLines[lineIndex + 1];
			textLines[lineIndex + 1] = line.mid(nbChar);
			textLines[lineIndex] = textLines[lineIndex] + " " + line.left(nbChar - 1);
		}
		else if (i == 444) {
			int lineIndex = 4;
			int nbChar = 7;
			auto line = textLines[lineIndex];
			textLines[lineIndex] = line.chopped(nbChar);
			textLines[lineIndex + 1] = line.right(nbChar - 1) + " " + textLines[lineIndex + 1];
		}
		else if (i == 344) {
			int lineIndex = 6;
			int nbChar = 6;
			auto line = textLines[lineIndex];
			textLines[lineIndex] = line.chopped(nbChar);
			textLines[lineIndex + 1] = line.right(nbChar - 1) + " " + textLines[lineIndex + 1];
		}

		quranText.append(textLines);
	}

	QString suraWord = "سُورَةُ";
	QString bism = "بِسْمِ ٱللَّهِ ٱلرَّحْمَٰنِ ٱلرَّحِيمِ";

	QString surapattern = "^("
		+ suraWord + " .*|"
		+ bism
		+ "|" + "بِّسْمِ ٱللَّهِ ٱلرَّحْمَٰنِ ٱلرَّحِيمِ"
		+ ")$";

	QRegularExpression re(surapattern, QRegularExpression::MultilineOption);
	for (auto textLines : PageAnalysisResult::quranText) {
		QVector<QStringList> page;

		for (int i = 0; i < textLines.size(); i++) {
			auto textLine = textLines[i];
			auto match = re.match(textLine);
			if (match.hasMatch()) {
				page.append(QStringList{});
				continue;
			}
			auto words = textLine.split(char(0x20), Qt::SkipEmptyParts);
			page.append(words);
		}
		QuranTextByWord.append(page);

	}
}


static bool isWaqfSad(const QPainterPath& path, AFont* font, double* distance = nullptr) {
	if (!font) return false;
	double exact = 1, scaled = 1;
	const bool matched = font->checkGlyph(path, "sad.waqf", CompareMethod::AREA,
	                                      constants::MATCHING_ERROR_RATIO, &exact) != -1;
	if (!matched) font->checkGlyphScaled(path, "sad.waqf", constants::MATCHING_ERROR_RATIO, &scaled);
	if (distance) *distance = qMin(exact, scaled);
	return matched || scaled < constants::MATCHING_ERROR_RATIO;
}

static bool checkGlyph(QGraphicsPathItem* pathItem, AFont* font, QGraphicsScene* scene, QSet<QGraphicsPathItem*>& removedItems) {
	auto itemPath = pathItem->path(); // *pathItem->sceneTransform();
	bool found = false;

	auto collidingItems = scene->items(pathItem->mapToScene(pathItem->path()), Qt::ContainsItemShape);
	for (auto colItem : collidingItems) {
		auto colItemPath = (QGraphicsPathItem*)colItem;
		auto newPath = pathItem->path();
		newPath.addPath(colItemPath->path());
		pathItem->setPath(newPath);
		removedItems.insert(colItemPath);
		found = true;
	}


	if (font->checkGlyph(itemPath, "twodots.waqf") != -1) {
		auto collidingItems = scene->items(pathItem->sceneBoundingRect(), Qt::IntersectsItemBoundingRect);
		if (collidingItems.size() == 2) {
			auto qafwaqf = (QGraphicsPathItem*)collidingItems.at(0);
			if (collidingItems.at(1) != pathItem) {
				qafwaqf = (QGraphicsPathItem*)collidingItems.at(1);
			}
			decltype(auto) newPath = qafwaqf->path();
			newPath.addPath(itemPath);
			qafwaqf->setPath(newPath);
			qafwaqf->setData((int)CustomData::NBPATH, 1);
			qafwaqf->setData((int)CustomData::TYPE, (int)CustomDataType::WAQF);
			removedItems.insert(pathItem);
			found = true;
		}
	}
	else if (isWaqfSad(itemPath, font)) {
		pathItem->setData((int)CustomData::TYPE, (int)CustomDataType::WAQF);
	}
	else if (font->checkGlyph(itemPath, "onedot.waqf") != -1) {
		auto collidingItems = scene->items(pathItem->sceneBoundingRect(), Qt::IntersectsItemBoundingRect);
		for (auto& collidingItem : collidingItems) {
			if (collidingItem == pathItem) continue;
			auto hahwaqf = (QGraphicsPathItem*)collidingItem;
			if (font->checkGlyph(hahwaqf->path(), "hah.waqf") != -1) {
				auto newPath = hahwaqf->path();
				newPath.addPath(itemPath);
				hahwaqf->setPath(newPath);
				hahwaqf->setData((int)CustomData::NBPATH, 1);
				hahwaqf->setData((int)CustomData::TYPE, (int)CustomDataType::WAQF);
				removedItems.insert(pathItem);
				found = true;
			}
		}
	}
	else if (font->checkGlyph(pathItem->path(), "onedot") != -1) {
		auto boundingRect = pathItem->sceneBoundingRect();
		boundingRect.setWidth(1.2 * boundingRect.width());

		auto items = scene->items(boundingRect, Qt::IntersectsItemShape);

		if (items.size() == 2) {
			QGraphicsPathItem* other = nullptr;
			if (items.at(0) == pathItem) {
				other = (QGraphicsPathItem*)items.at(1);
			}
			else {
				other = (QGraphicsPathItem*)items.at(0);
			}
			if (font->checkGlyph(other->path(), "onedot") != -1) {
				//join the two dots
				auto path = pathItem->path();
				path.addPath(other->path());
				pathItem->setPath(path);
				//line.remove(other);
				removedItems.insert(other);
			}
		}
	}
	else {
		//check fathatan, kasratan, fathatanidgham, kasratanidgham
		auto boundingRect = pathItem->sceneBoundingRect();
		boundingRect.setHeight(boundingRect.height() + 1);


		auto items = scene->items(boundingRect, Qt::IntersectsItemShape);

		if (items.size() == 2) {
			QGraphicsPathItem* other = nullptr;
			if (items.at(0) == pathItem) {
				other = (QGraphicsPathItem*)items.at(1);
			}
			else {
				other = (QGraphicsPathItem*)items.at(0);
			}
			if (font->scaleAndCompareQPainterPath(pathItem->path(), other->path(), true, true) < constants::MATCHING_ERROR_RATIO) {
				QRectF otherBoundingRect = other->sceneBoundingRect();

				if (std::abs(otherBoundingRect.x() - boundingRect.x()) < 3.5) {
					pathItem->setData((int)CustomData::NBPATH, 1);
					pathItem->setData((int)CustomData::TYPE, (int)CustomDataType::SIMILAR);
					auto path = pathItem->path();
					path.addPath(other->path());
					pathItem->setPath(path);
					removedItems.insert(other);
					//qDebug() << "boundingRect:" << boundingRect << ",otherBoundingRect : " << otherBoundingRect << "\n";
				}
			}
		}
	}

	return found;
}
void PageAnalysisResult::setLinesInfo(int pageNumber, AFont* font, QGraphicsScene* scene) {



	auto baseline = firstliney;
	if (pageNumber < 3) {
		baseline = 146;
	}


	for (int i = 0; i < LinesInfo.size(); i++) {


		if (i == 1 && pageNumber < 3) {
			baseline = 210;
		}

		LinesInfo[i].baseline = baseline;

		if (i == 0 || LinesInfo[i].isSura || (!LinesInfo[i].isSura && LinesInfo[i - 1].isSura))
		{
			QRectF rec{ left , baseline - ascendant , lineWidth , ascendant + desendant };
			auto items = scene->items(rec, Qt::IntersectsItemBoundingRect);

			double maxArea = -1;
			double bestLine = 0.0;
			for (float yline = baseline - ascendant; yline <= baseline + desendant; yline++) {
				QRectF line{ left , yline, lineWidth , 4 };
				QPainterPath  linePath;
				linePath.addRect(line);
				auto area = 0.0;
				for (auto& item : items) {
					QGraphicsPathItem* pathItem = (QGraphicsPathItem*)item;
					auto transform = pathItem->sceneTransform();
					auto path = pathItem->path() * transform;
					auto result = path.intersected(linePath);
					area += font->getArea(result);
				}
				if (maxArea < area) {
					bestLine = line.y() + line.height() / 2;
					maxArea = area;
				}
			}
			baseline = bestLine;
			LinesInfo[i].baseline = baseline;
		}

		if (LinesInfo[i].isSura) {

			LinesInfo[i].descendant = desendant - 2;
			LinesInfo[i].ascendant = ascendant - 2;

			for (float yline = baseline; yline >= baseline - 25; yline--) {
				QRectF line{ left , yline, lineWidth , 1 };
				auto intesects = scene->items(line, Qt::IntersectsItemBoundingRect);
				if (intesects.size() == 0) {
					LinesInfo[i].ascendant = baseline - yline;
					break;
				}
			}

			for (float yline = baseline; yline <= baseline + 25; yline++) {
				QRectF line{ left , yline, lineWidth , 1 };
				auto intesects = scene->items(line, Qt::IntersectsItemBoundingRect);
				if (intesects.size() == 0) {
					LinesInfo[i].descendant = yline - baseline;
					break;
				}
			}
		}


		baseline += interline;

	}
	// Acendant , descendant
	for (int i = 0; i < LinesInfo.size(); i++) {

		if (!LinesInfo[i].isSura) {
			LinesInfo[i].descendant = desendant;
			LinesInfo[i].ascendant = ascendant;

			if (i == 0) {
				LinesInfo[i].ascendant = ascendant + ascendant;
			}

			if (i < (LinesInfo.size() - 1) && LinesInfo[i + 1].isSura) {
				LinesInfo[i].descendant = (LinesInfo[i + 1].baseline - LinesInfo[i].baseline) - LinesInfo[i + 1].ascendant;
			}

			if (i > 0 && LinesInfo[i - 1].isSura) {
				LinesInfo[i].ascendant = (LinesInfo[i].baseline - LinesInfo[i - 1].baseline) - LinesInfo[i - 1].descendant;
			}

			if (i == (LinesInfo.size() - 1)) {
				LinesInfo[i].descendant = desendant + desendant;
			}
		}
	}
}

void PageAnalysisResult::debugLines(int pageNumber, AFont* font, QGraphicsScene* scene) {

	QVector<QSet<QGraphicsPathItem*>> lines;



	setLinesInfo(pageNumber, font, scene);

	QSet<QGraphicsPathItem*> itemsFound;
	QSet<QGraphicsPathItem*> prevLineItems;
	for (int i = 0; i < LinesInfo.size(); i++) {

		QRectF rec{ left , LinesInfo[i].baseline - LinesInfo[i].ascendant , lineWidth , LinesInfo[i].ascendant + LinesInfo[i].descendant };
		auto items = scene->items(rec, Qt::IntersectsItemBoundingRect);


		QSet<QGraphicsPathItem*> currentLineItems;

		for (auto& item : items) {

			QGraphicsPathItem* pathItem = (QGraphicsPathItem*)item;
			if (prevLineItems.contains(pathItem)) continue;

			auto boundingrect = pathItem->sceneBoundingRect();

			if (i == (LinesInfo.size() - 1)) {
				itemsFound.insert(pathItem);
				currentLineItems.insert(pathItem);
			}
			else {
				auto dycurrent = (LinesInfo[i].baseline + LinesInfo[i].descendant) - boundingrect.top();
				auto dynext = boundingrect.bottom() - (LinesInfo[i + 1].baseline - LinesInfo[i + 1].ascendant);

				//auto dycurrent = (baseline + interline / 2) - boundingrect.top();
				//auto dynext = boundingrect.bottom() - (baseline + interline / 2);

				if (dycurrent >= dynext || LinesInfo[i + 1].isSura) {
					itemsFound.insert(pathItem);
					currentLineItems.insert(pathItem);
				}
			}

		}
		lines.append(currentLineItems);
		prevLineItems = currentLineItems;
	}

	for (auto& item : scene->items(scene->sceneRect())) {
		QGraphicsPathItem* pathItem = (QGraphicsPathItem*)item;
		if (!itemsFound.contains(pathItem)) {
			QSet<QGraphicsPathItem*> removedItems;
			bool found = checkGlyph(pathItem, font, scene, removedItems);
			if (!found) {
				pathItem->setBrush(Qt::red);
				NotMatched notmatched;
				notmatched.item = Apath{ pathItem->path(),"",pathItem->data((int)CustomData::NBPATH).toInt(),(CustomDataType)pathItem->data((int)CustomData::TYPE).toInt(),-1,pathItem->transform(),pathItem->pos() };
				notMatchedItems.append(notmatched);
			}
			for (auto& item : removedItems) {
				scene->removeItem(item);
			}
		}
	}


	analyzePage(pageNumber, font, scene, lines);

	// Draw baseline, ascent, descent
	QPen penGreen(Qt::green, 0);
	QPen penAcent(Qt::blue, 0);
	QPen penDesent(Qt::red, 0);
	for (int i = 0; i < LinesInfo.size(); i++) {

		auto line = new QGraphicsLineItem(left, LinesInfo[i].baseline, left + lineWidth, LinesInfo[i].baseline);
		line->setPen(penGreen);
		scene->addItem(line);


		line = new QGraphicsLineItem(left, LinesInfo[i].baseline - LinesInfo[i].ascendant, left + lineWidth, LinesInfo[i].baseline - LinesInfo[i].ascendant);
		line->setPen(penAcent);
		scene->addItem(line);

		line = new QGraphicsLineItem(left, LinesInfo[i].baseline + LinesInfo[i].descendant, left + lineWidth, LinesInfo[i].baseline + LinesInfo[i].descendant);
		line->setPen(penDesent);
		scene->addItem(line);

	}

	for (auto line : lines) {
		ALine newline;
		Word newWord;
		for (auto word : line) {
			newWord.paths.append(Apath{ word->path(),"",word->data((int)CustomData::NBPATH).toInt(),(CustomDataType)word->data((int)CustomData::TYPE).toInt(),-1,word->transform(),word->pos() });
		}
		newline.words.append(newWord);
		page.lines.append(newline);
	}
}

void PageAnalysisResult::detectWords(int pageNumber, AFont* font, QGraphicsScene* scene) {

	std::multimap<qreal, QGraphicsPathItem*> upperBRTopBottom;

	QVector<QVector<QVector<QGraphicsPathItem*>>> lines;

	for (auto& item : scene->items(scene->sceneRect())) {
		QGraphicsPathItem* pathItem = (QGraphicsPathItem*)item;
		auto brect = pathItem->sceneBoundingRect();
		upperBRTopBottom.insert({ brect.y(), pathItem });
		//upperBRTopBottom.insert({ brect.y() + brect.height(), pathItem });
	}

	auto current = upperBRTopBottom.begin();

	for (int i = 0; i < LinesInfo.size(); i++) {

		QVector<QVector<QGraphicsPathItem*>> line;

		if (LinesInfo[i].isSura && current != upperBRTopBottom.end()) {
			lines.append(line);

			bool bismLine = i != 0 && LinesInfo[i - 1].isSura;

			int nbBismShapes = 31;

			if ((pageNumber == 597 && i == 3) || (pageNumber == 598 && i == 4)) {
				nbBismShapes = 32;
			}

			if (bismLine) {
				for (int ip = 0; ip < nbBismShapes; ip++) {
					auto item = current->second;
					headingPaths[i].append(item->mapToScene(item->path()));
					scene->removeItem(current->second);
					current++;
				}
			}
			else {
				QSet<QGraphicsItem*> removedItems;


				QGraphicsPathItem* pathItem = current->second;
				auto br = pathItem->sceneBoundingRect();

				auto yline = br.y();
				while (true) {
					QRectF line{ left , yline, lineWidth , 1 };
					auto intesects = scene->items(line, Qt::IntersectsItemBoundingRect);
					if (intesects.size() == 0) break;
					for (auto item : intesects) {
						removedItems.insert(item);
					}
					yline++;
				}
				/*
					if (bismLine) {
						std::cout << "Nb path bism line " << removedItems.size() << std::endl;
					}**/

				for (auto& item : removedItems) {
					auto pathItem = static_cast<QGraphicsPathItem*>(item);
					headingPaths[i].append(pathItem->mapToScene(pathItem->path()));
					scene->removeItem(item);
					current++;
				}
			}



			continue;
		}

		auto& lineInfo = LinesInfo[i];
		auto totalLinePath = 0;
		for (auto word : lineInfo.wordInfos) {
			totalLinePath += word.nbContoursByByWord;
		}
		std::multimap<qreal, QGraphicsPathItem*, std::greater <qreal> > rightBR;
		int skipped = 0;

		/*int indexCheckWaqf = totalLinePath - 3;
		if (pageNumber == 443 && (i == 10 || i == 11)) {
			indexCheckWaqf = totalLinePath - 7;
		}*/
		int indexCheckWaqf = totalLinePath - 6;
		if (pageNumber == 40 && (i == 1)) {
			indexCheckWaqf = totalLinePath - 10;
		}
		auto done = 0;
		for (int pathIndex = 0; pathIndex < totalLinePath && current != upperBRTopBottom.end(); pathIndex++) {

			if (pathIndex >= indexCheckWaqf && current->second->data((int)CustomData::TYPE).toInt() == (int)CustomDataType::WAQF) {
				pathIndex--;
				current++;
				skipped++;
			}
			else if (!done && pageNumber == 389 && i == 3 && pathIndex == (totalLinePath - 1)) {
				pathIndex--;
				current++;
				skipped++;
				done++;
			}
			else if (!done && pageNumber == 260 && i == 0 && pathIndex == (totalLinePath - 1)) {
				pathIndex--;
				current++;
				skipped++;
				done++;
			}
			else if (done < 2 && pageNumber == 439 && i == 9 && pathIndex == (totalLinePath - 1)) {
				pathIndex--;
				current++;
				skipped++;
				done++;
			}
			else {
				QGraphicsPathItem* pathItem = current->second;
				auto brect = pathItem->sceneBoundingRect();
				rightBR.insert({ brect.x() + brect.width(), pathItem });
				pathIndex += pathItem->data((int)CustomData::NBPATH).toInt();
				current = upperBRTopBottom.erase(current);
				//current++;
			}
		}
		std::advance(current, -skipped);
		auto currentShape = rightBR.begin();
		auto wordIndex = 0;
		for (auto wordInfo : lineInfo.wordInfos) {
			QVector<QGraphicsPathItem*> word;
			for (int pathIndex = 0; pathIndex < wordInfo.nbContoursByByWord && currentShape != rightBR.end(); pathIndex++) {

				if (pathIndex + currentShape->second->data((int)CustomData::NBPATH).toInt() == wordInfo.nbContoursByByWord) {
					QGraphicsPathItem* last = word.last();
					auto brect = last->sceneBoundingRect();
					word.removeLast();
					word.append(currentShape->second);
					currentShape = rightBR.erase(currentShape);
					rightBR.insert({ brect.x() + brect.width(), last });
					currentShape = rightBR.begin();
				}
				else {
					word.append(currentShape->second);
					pathIndex += currentShape->second->data((int)CustomData::NBPATH).toInt();
					currentShape = rightBR.erase(currentShape);
					//currentShape++;
				}
			}
			if ((pageNumber == 548 && i == 10 && wordIndex == 1) || (pageNumber == 379 && i == 9 && wordIndex == 2) || (pageNumber == 1 && i == 1 && wordIndex == 2)) {

			}
			else if (currentShape != rightBR.end()) {
				QRectF wordRect;
				qreal farX = 1000;
				int lastIndex = -1;

				for (int itemI = 0; itemI < word.size(); itemI++) {
					auto item = word[itemI];
					auto brect = item->sceneBoundingRect();
					if (brect.x() < farX) {
						farX = brect.x();
						lastIndex = itemI;
					}
					wordRect = wordRect.united(brect);
				}
				QGraphicsPathItem* last = word[lastIndex];
				auto brect = last->sceneBoundingRect();
				auto curect = currentShape->second->sceneBoundingRect();
				auto diff = curect.x() - brect.x();
				// && !(pageNumber == 543 && i == 4 && wordIndex == 3
				if (diff > 5) {
					//if (pageNumber == 543 && i == 4 && wordIndex == 3) {
					//	std::cout << wordIndex << std::endl;
					//}
					//else {
					word.removeAt(lastIndex);
					word.append(currentShape->second);
					currentShape = rightBR.erase(currentShape);
					rightBR.insert({ brect.x() + brect.width(), last });
					currentShape = rightBR.begin();
					//}

				}
			}

			line.append(word);
			wordIndex++;
		}
		lines.append(line);
	}

	for (auto line : lines) {
		ALine newline;
		for (auto words : line) {
			Word newword;
			for (auto word : words) {
				newword.paths.append(Apath{ word->path(),"",word->data((int)CustomData::NBPATH).toInt(),(CustomDataType)word->data((int)CustomData::TYPE).toInt(),-1,word->transform(),word->pos() });
			}
			newline.words.append(newword);
		}
		newline.baseline = detectBaseline(newline, font);
		//auto baseline = detectBaseline2(newline, font);
		//std::cout << "newline.baseline  " << newline.baseline << ",baseline = " << baseline;

		page.lines.append(newline);
	}
}
namespace {
QPainterPath boundaryWorldPath(const Apath& shape) {
	auto path = shape.transform.map(shape.path);
	path.translate(shape.pos);
	return path;
}

bool boundaryHasLetters(const QString& text) {
	for (const auto ch : text)
		if (ch.isLetter() && (ch.unicode() < 0x06D6 || ch.unicode() > 0x06ED)) return true;
	return false;
}

struct BoundaryWord {
	QRectF body;
	QSet<int> bodies;
	QVector<QRectF> boxes;
	QVector<int> lowerStrokes;
	int expectedLower = 0;
	int observedLower = 0;
	bool valid = true;
};

BoundaryWord boundaryWord(const Word& word, const QString& text, qreal baseline, AFont* font) {
	BoundaryWord result;
	result.expectedLower = text.count(QChar(0x0650)) + 2 * text.count(QChar(0x064D));
	result.valid = !(int(word.wordResultInfo.state) &
		(int(WordResultFlags::DIFFERENT_PATH_NUMBER) | int(WordResultFlags::NBPATH_OVERFLOW)));
	for (const auto& sub : word.wordResultInfo.subWords) {
		if (sub.paths.empty() || !boundaryHasLetters(sub.text)) continue;
		const int index = sub.paths.front();
		if (index < 0 || index >= word.paths.size()) { result.valid = false; continue; }
		result.bodies.insert(index);
		if (sub.secondaryBodyPath >= 0) result.bodies.insert(sub.secondaryBodyPath);
		result.body = result.body.united(boundaryWorldPath(word.bodyShape(sub)).boundingRect());
	}
	for (int index = 0; index < word.paths.size(); ++index) {
		const auto& shape = word.paths[index];
		const auto box = boundaryWorldPath(shape).boundingRect();
		result.boxes.append(box);
		if (result.bodies.contains(index) || box.center().y() <= baseline) continue;
		if (int(shape.type) == 0 && shape.nbpath == 0 && box.width() > box.height() &&
			font->checkGlyph(shape.path, QChar(0x064E), constants::MATCHING_ERROR_RATIO) != -1) {
			result.lowerStrokes.append(index);
			++result.observedLower;
		}
		else if (shape.type == CustomDataType::SIMILAR) {
			// Count a packed kasratan only when both outlines match the stroke template.
			const auto parts = shape.path.toSubpathPolygons();
			bool strokes = parts.size() == 2 && shape.nbpath == 1;
			for (const auto& part : parts) {
				QPainterPath path; path.addPolygon(part); path.closeSubpath();
				strokes = strokes && font->checkGlyph(path, QChar(0x064E), constants::MATCHING_ERROR_RATIO) != -1;
			}
			if (strokes) result.observedLower += 2;
			else result.valid = false; // Grouping uncertainty cannot justify moving another mark.
		}
	}
	result.valid = result.valid && !result.body.isEmpty();
	return result;
}
}

QJsonArray PageAnalysisResult::resolveWordBoundaries(int pageNumber, AFont* font) {
	QJsonArray repairs;
	if (!font || pageNumber < 1 || pageNumber > QuranTextByWord.size()) return repairs;
	const auto& transcript = QuranTextByWord[pageNumber - 1];
	for (int li = 0; li < page.lines.size() && li < transcript.size(); ++li) {
		auto& line = page.lines[li];
		if (line.words.size() != transcript[li].size() || line.baseline <= 0) continue;
		QVector<BoundaryWord> evidence;
		for (int wi = 0; wi < line.words.size(); ++wi) {
			detectSubWords(pageNumber - 1, li, wi, font);
			evidence.append(boundaryWord(line.words[wi], transcript[li][wi], line.baseline, font));
		}
		for (int wi = 0; wi + 1 < line.words.size(); ++wi) {
			const auto& right = evidence[wi];
			const auto& left = evidence[wi + 1];
			if (!right.valid || !left.valid) continue;
			const int rightDebt = right.observedLower - right.expectedLower;
			const int leftDebt = left.observedLower - left.expectedLower;
			if (rightDebt * leftDebt != -1) continue;
			const qreal gap = right.body.left() - left.body.right();
			// Body order must already be sound. Do not repair overlapping letter bodies here.
			if (gap < 0) continue;
			const qreal boundary = (right.body.left() + left.body.right()) / 2;
			auto nearBoundary = [=](const QRectF& box) {
				return qAbs(box.center().x() - boundary) <= qMax(box.width(), box.height()) + gap / 2;
			};
			const int overIndex = rightDebt == 1 ? wi : wi + 1;
			const int underIndex = rightDebt == 1 ? wi + 1 : wi;
			const auto& over = evidence[overIndex];
			const auto& under = evidence[underIndex];
			QVector<int> lower, upper;
			for (int pi : over.lowerStrokes)
				if (nearBoundary(over.boxes[pi])) lower.append(pi);
			for (int pi = 0; pi < under.boxes.size(); ++pi) {
				const auto& shape = line.words[underIndex].paths[pi];
				if (!under.bodies.contains(pi) && int(shape.type) == 0 && shape.nbpath == 0 &&
					under.boxes[pi].bottom() < line.baseline && nearBoundary(under.boxes[pi]) &&
					font->checkGlyph(shape.path, "onedot") == -1 &&
					font->checkGlyph(shape.path, "twodots") == -1) upper.append(pi);
			}
			// Counts alone cannot choose ink: require one unique candidate on each side.
			if (lower.size() != 1 || upper.size() != 1) continue;
			const int lowerPath = lower.front(), upperPath = upper.front();
			std::swap(line.words[overIndex].paths[lowerPath], line.words[underIndex].paths[upperPath]);
			for (int index : {overIndex, underIndex}) {
				detectSubWords(pageNumber - 1, li, index, font, true);
				evidence[index] = boundaryWord(line.words[index], transcript[li][index], line.baseline, font);
			}
			repairs.append(QJsonObject{{"line",li + 1},{"lowerFromWord",overIndex + 1},
				{"lowerPath",lowerPath},{"upperFromWord",underIndex + 1},{"upperPath",upperPath},
				{"reason","reciprocal lower-stroke quota; unique boundary exchange"}});
		}
	}
	return repairs;
}

qreal PageAnalysisResult::detectBaseline(const ALine& line, AFont* font) {
	QPainterPath linePath;
	for (int wordIndex = 0; wordIndex < line.words.length(); wordIndex++) {
		auto& word = line.words[wordIndex];
		for (auto& shape : word.paths) {

			auto pos = shape.pos;
			auto itemPath = shape.path;
			itemPath = itemPath * shape.transform;
			itemPath.translate(pos.x(), pos.y());
			linePath.addPath(itemPath);

		}
	}

	auto bbox = linePath.boundingRect();
	qreal maxArea = 0.0;
	qreal bestLine = 0.0;
	for (qreal yline = bbox.top(); yline <= bbox.bottom(); yline = yline + 0.2) {
		QRectF line{ bbox.left() , yline, bbox.width() , 1.3 };
		QPainterPath  baseLinePath;
		baseLinePath.addRect(line);
		auto result = linePath.intersected(baseLinePath);
		auto area = font->getArea(result);
		if (maxArea < area) {
			bestLine = line.y() + line.height() / 2;
			maxArea = area;
		}
	}

	return bestLine;
}
qreal PageAnalysisResult::detectBaseline2(const ALine& line, AFont* font) {
	/*
	QPainterPath linePath;
	for (int wordIndex = 0; wordIndex < line.words.length(); wordIndex++) {
		auto& word = line.words[wordIndex];
		for (auto& shape : word.paths) {

			auto pos = shape.pos;
			auto itemPath = shape.path;
			itemPath = itemPath * shape.transform;
			itemPath.translate(pos.x(), pos.y());
			linePath.addPath(itemPath);

		}
	}

	auto bbox = linePath.boundingRect();*/

	QRectF bbox;
	for (int wordIndex = 0; wordIndex < line.words.length(); wordIndex++) {
		auto& word = line.words[wordIndex];
		for (auto& shape : word.paths) {

			auto pos = shape.pos;
			auto itemPath = shape.path;
			itemPath = itemPath * shape.transform;
			itemPath.translate(pos.x(), pos.y());
			bbox = bbox.united(itemPath.boundingRect());
		}
	}


	qreal maxArea = 0.0;
	qreal bestLine = 0.0;
	for (qreal yline = bbox.top(); yline <= bbox.bottom(); yline = yline + 0.2) {
		QRectF rect{ bbox.left() , yline, bbox.width() , 1.3 };
		QPainterPath  baseLinePath;
		baseLinePath.addRect(rect);
		//auto result = linePath.intersected(baseLinePath);
		qreal area = 0.0;
		for (int wordIndex = 0; wordIndex < line.words.length(); wordIndex++) {
			auto& word = line.words[wordIndex];
			for (auto& shape : word.paths) {

				auto pos = shape.pos;
				auto itemPath = shape.path;
				itemPath = itemPath * shape.transform;
				itemPath.translate(pos.x(), pos.y());
				//linePath.addPath(itemPath);
				auto result = itemPath.intersected(baseLinePath);
				area += font->getArea(result);

			}
		}
		if (maxArea < area) {
			bestLine = rect.y() + rect.height() / 2;
			maxArea = area;
		}
	}

	return bestLine;
}

int PageAnalysisResult::expectedLowerPathCount(ushort u, arabic_joining_t joining) {
    if (u==0x0650 || u==0x0655 || u==0x0625 || u==0x06ED || u==0x06E3) return 1;
    if (u==0x064D || u==0x08F2) return 2;
    if (u==0x0628 || u==0x062C) return 1;
    if (u==0x064A && (joining==INIT || joining==MEDI)) return 1;
    return 0;
}

namespace {
enum class NativeMarkFamily { Unknown, Stroke, Damma, SingleDot, DoubleDot };

int lastBoundaryLetter(const QString& text) {
    for (int i=text.size()-1; i>=0; --i)
        if (text[i].isLetter() && text[i].unicode()!=0x0640 &&
            (text[i].unicode()<0x06D6 || text[i].unicode()>0x06ED)) return i;
    return -1;
}

int expectedLowerPaths(const QString& text) {
    int count=0;
    const auto joins=PageAnalysisResult::arabic_joining(text);
    for (int i=0; i<text.size(); ++i) {
        count+=PageAnalysisResult::expectedLowerPathCount(text[i].unicode(),joins[i].joining);
    }
    return count;
}

int expectedLowerStrokePaths(const QString& text) {
    return text.count(QChar(0x0650)) + 2 * (text.count(QChar(0x064D)) + text.count(QChar(0x08F2)));
}
}

void PageAnalysisResult::repairSubwordBoundaries(int pageIndex, int lineIndex, int wordIndex, AFont* font) {
    if (!font) return;
    auto& line=page.lines[lineIndex];
    auto& word=line.words[wordIndex];
    auto& subs=word.wordResultInfo.subWords;
    if (subs.size()<2 || (int(word.wordResultInfo.state) &
        (int(WordResultFlags::DIFFERENT_PATH_NUMBER)|int(WordResultFlags::NBPATH_OVERFLOW)))) return;
    QString joined;
    for (const auto& sub:subs) joined+=sub.text;
    if (joined!=word.text) return;
    bool applicable=false;
    for (const auto& sub:subs) {
        const int last=lastBoundaryLetter(sub.text);
        if (last<0) continue;
        const auto u=sub.text[last].unicode();
        const auto tail=sub.text.mid(last+1);
        applicable|=(u==0x0623 || (u==0x0627 && tail.contains(QChar(0x0653))) ||
                                  (u==0x0648 && tail.contains(QChar(0x0670))));
    }
    if (!applicable) return;
    QVector<QRectF> boxes;
    QVector<int> owners(word.paths.size(),-1), families(word.paths.size(),-1);
    QSet<int> bodies;
    for (int si=0; si<int(subs.size()); ++si) {
        if (subs[si].paths.empty()) return;
        if (boundaryHasLetters(subs[si].text)) bodies.insert(subs[si].paths.front());
        for (int pi:subs[si].paths) {
            if (pi<0 || pi>=word.paths.size() || owners[pi]!=-1) return;
            owners[pi]=si;
        }
    }
    for (const auto& path:word.paths) boxes.append(boundaryWorldPath(path).boundingRect());
    auto eligible=[&](int pi) {
        return !bodies.contains(pi) && word.paths[pi].type!=CustomDataType::WAQF && word.paths[pi].nbpath==0;
    };
    auto lowerCount=[&](int si) {
        int n=0;
        for (int pi:subs[si].paths)
            if (!bodies.contains(pi) && word.paths[pi].type!=CustomDataType::WAQF &&
                boxes[pi].center().y()>line.baseline) n+=1+word.paths[pi].nbpath;
        return n;
    };
    auto family=[&](int pi) {
        if (families[pi]>=0) return NativeMarkFamily(families[pi]);
        const auto& path=word.paths[pi].path;
        const auto box=boxes[pi];
        const qreal ratio=box.width()/qMax(box.height(),qreal(.0001));
        NativeMarkFamily f=NativeMarkFamily::Unknown;
        if (ratio>1.3 && font->checkGlyph(path,QChar(0x064E),constants::MATCHING_ERROR_RATIO)!=-1) f=NativeMarkFamily::Stroke;
        else if (ratio>.8 && ratio<1.2 && font->checkGlyph(path,"onedot")!=-1) f=NativeMarkFamily::SingleDot;
        else if (ratio>1.2 && ratio<2.1 && font->checkGlyph(path,"twodots")!=-1) f=NativeMarkFamily::DoubleDot;
        else if (ratio>.7 && ratio<1.1 && font->checkGlyph(path,"damma")!=-1) f=NativeMarkFamily::Damma;
        families[pi]=int(f); return f;
    };
    for (int si=0; si+1<int(subs.size()); ++si) {
        auto& right=subs[si]; auto& left=subs[si+1];
        if (!bodies.contains(right.paths.front()) || !bodies.contains(left.paths.front())) continue;
        const int last=lastBoundaryLetter(right.text);
        if (last<0) continue;
        const auto tail=right.text.mid(last+1);
        const auto rb=boxes[right.paths.front()], lb=boxes[left.paths.front()];
        if (rb.center().x()<=lb.center().x()) continue;
        const qreal gap=rb.left()-lb.right(), boundary=(rb.left()+lb.right())/2;
        auto near=[&](int pi) {
            const auto b=boxes[pi];
            return qAbs(b.center().x()-boundary)<=qMax(b.width(),b.height())+qMax(qreal(0),gap)/2;
        };
        auto exchange=[&](int fromRight,int fromLeft,const QString& reason) {
            auto a=std::find(right.paths.begin(),right.paths.end(),fromRight);
            auto b=std::find(left.paths.begin(),left.paths.end(),fromLeft);
            if (a==right.paths.end() || b==left.paths.end()) return;
            std::swap(*a,*b); owners[fromRight]=si+1; owners[fromLeft]=si;
            subwordBoundaryRepairs.append(QJsonObject{{"page",pageIndex+1},{"line",lineIndex+1},{"word",wordIndex+1},
                {"rightSubword",si+1},{"leftSubword",si+2},{"pathToLeft",fromRight},{"pathToRight",fromLeft},{"reason",reason}});
        };
        const int excess=lowerCount(si)-expectedLowerPaths(right.text);
        if (right.text[last].unicode()==0x0623) {
            const bool stroke=tail.contains(QChar(0x064E)) || tail.contains(QChar(0x0653));
            const bool damma=tail.contains(QChar(0x064F));
            const bool sukun=tail.contains(QChar(0x0652)) || tail.contains(QChar(0x06E1));
            if (!stroke && !damma && !sukun) continue;
            QVector<int> possible;
            for (int side:{si,si+1}) for (int pi:subs[side].paths) {
                if (!eligible(pi)) continue;
                const auto b=boxes[pi]; const qreal ratio=b.width()/qMax(b.height(),qreal(.0001));
                if (ratio>=.8 && ratio<=1.3 && b.height()>=.1*rb.height() && b.height()<=.4*rb.height() &&
                    qAbs(b.center().x()-rb.left())<=b.width() && qAbs(b.bottom()-rb.top())<=b.height()) possible.append(pi);
            }
            // A healthy stack needs no template work. Only a displaced lower
            // path or a hamza on the neighbour can justify an exchange.
            if (excess!=1 && std::all_of(possible.begin(),possible.end(),[&](int pi){return owners[pi]==si;})) continue;
            QVector<int> anchors;
            for (int pi:possible) if (family(pi)==NativeMarkFamily::Unknown) anchors.append(pi);
            if (anchors.size()!=1) continue;
            const int anchor=anchors.front(); const auto ab=boxes[anchor];
            if (owners[anchor]==si && excess==1) {
                QVector<int> lower,upper;
                for (int pi:right.paths) if (eligible(pi) && near(pi) && boxes[pi].center().y()>line.baseline) lower.append(pi);
                for (int pi:left.paths) {
                    if (!eligible(pi) || !near(pi) || boxes[pi].bottom()>ab.top()+.3*ab.height()) continue;
                    const auto f=family(pi);
                    if ((stroke && f==NativeMarkFamily::Stroke) || (damma && f==NativeMarkFamily::Damma) ||
                        (sukun && f==NativeMarkFamily::Unknown && boxes[pi].width()>boxes[pi].height())) upper.append(pi);
                }
                if (lower.size()==1 && upper.size()==1) exchange(lower.front(),upper.front(),"alif hamza/vowel stack; neighbour lower mark");
            }
            else if (owners[anchor]==si+1 && stroke) {
                QVector<int> outgoing;
                int rightStrokes=0,leftStrokes=0;
                for (int side:{si,si+1}) for (int pi:subs[side].paths)
                    if (eligible(pi) && boxes[pi].center().y()<line.baseline && family(pi)==NativeMarkFamily::Stroke) {
                        if (side==si) {
                            ++rightStrokes;
                            if (near(pi) && boxes[pi].top()>ab.bottom()) outgoing.append(pi);
                        } else ++leftStrokes;
                    }
                if (outgoing.size()==1 && rightStrokes==right.text.count(QChar(0x064E))+1 &&
                    leftStrokes+1==left.text.count(QChar(0x064E))) exchange(outgoing.front(),anchor,"hamza anchor above alif; neighbour fatha below stack");
            }
        }
        else if (excess==1) {
            const bool maddah=right.text[last].unicode()==0x0627 && tail.contains(QChar(0x0653));
            const bool smallAlif=right.text[last].unicode()==0x0648 && tail.contains(QChar(0x0670));
            if (!maddah && !smallAlif) continue;
            const bool reciprocalLower=lowerCount(si+1)+1==expectedLowerPaths(left.text);
            // A hamza above a low tooth may itself sit below the line baseline.
            // It must not satisfy a missing kasra quota. For an alif maddah,
            // also permit a reciprocal deficit of geometrically recognized
            // lower strokes. Keep the broader existing rule for other cases.
            auto lowerStrokes=[&](int side) {
                int count=0;
                for (int pi:subs[side].paths)
                    if (eligible(pi) && boxes[pi].center().y()>line.baseline &&
                        family(pi)==NativeMarkFamily::Stroke) ++count;
                return count;
            };
            const bool reciprocalStroke=!reciprocalLower && maddah &&
                lowerStrokes(si)==expectedLowerStrokePaths(right.text)+1 &&
                lowerStrokes(si+1)+1==expectedLowerStrokePaths(left.text);
            if (!reciprocalLower && !reciprocalStroke) continue;
            auto modifier=[&](int pi) {
                const auto b=boxes[pi];
                return eligible(pi) && b.center().y()<line.baseline && family(pi)==NativeMarkFamily::Unknown &&
                    ((maddah && b.width()>1.6*b.height()) || (smallAlif && b.height()>2*b.width()));
            };
            bool already=false;
            for (int pi:right.paths) if (modifier(pi)) already=true;
            if (already) continue;
            QVector<int> lower,upper;
            for (int pi:right.paths)
                if (eligible(pi) && near(pi) && boxes[pi].center().y()>line.baseline &&
                    (!reciprocalStroke || family(pi)==NativeMarkFamily::Stroke)) lower.append(pi);
            for (int pi:left.paths) {
                const auto b=boxes[pi];
                if (near(pi) && modifier(pi) && (!reciprocalStroke ||
                    (b.bottom()<rb.top() && b.left()<rb.right() && b.right()>rb.left()))) upper.append(pi);
            }
            if (lower.size()==1 && upper.size()==1) exchange(lower.front(),upper.front(),reciprocalStroke ?
                "raised alif maddah; reciprocal lower-stroke quota" : "raised alif modifier; reciprocal lower-mark quota");
        }
    }
}

void PageAnalysisResult::restoreWaqfTypes(int pageIndex, int lineIndex, int wordIndex, AFont* font) {
	if (!font) return;
	auto& word = page.lines[lineIndex].words[wordIndex];
	for (int pi = 0; pi < word.paths.size(); ++pi) {
		auto& shape = word.paths[pi];
		if (int(shape.type) != 0 || shape.nbpath != 0) continue;
		double distance = 1;
		if (!isWaqfSad(shape.path, font, &distance)) continue;
		shape.type = CustomDataType::WAQF;
		waqfTypeRepairs.append(QJsonObject{{"page",pageIndex+1},{"line",lineIndex+1},
			{"word",wordIndex+1},{"path",pi},{"previousType",0},{"type",int(CustomDataType::WAQF)},
			{"template","sad.waqf"},{"distance",distance},
			{"reason","native stop-sign template; uniform scale fallback"}});
	}
}

ShapeExceptionRecord PageAnalysisResult::wordShapeException(int page, int line, int word, int wordsInLine) {
    const auto pageIt = nbShapeExceptions.constFind(page);
    if (pageIt == nbShapeExceptions.constEnd()) return {};
    const auto lineIt = pageIt->constFind(line);
    if (lineIt == pageIt->constEnd()) return {};
    if (lineIt->contains(word)) return lineIt->value(word);
    if (word < 1 || word > wordsInLine) return {};
    return lineIt->value(word - wordsInLine - 1, {});
}

static bool endsInLamAlef(const QString& text) {
    int i = text.size() - 1;
    while (i >= 0 && text[i].isMark()) --i;
    if (i < 0 || !QStringLiteral("اأإآٱ").contains(text[i])) return false;
    --i;
    while (i >= 0 && text[i].isMark()) --i;
    return i >= 0 && text[i] == QChar(0x0644);
}

// The separate arm of a final lam-alef tapers from the upper left to the lower right.
// Measure the actual ink in the outer thirds so a nearby vowel or dot stays a mark.
static bool isLamAlefArm(const QRegion& ink) {
    const auto bounds = ink.boundingRect();
    const int third = bounds.height() / 3;
    if (!third || bounds.width() < .6 * bounds.height() || bounds.width() > 1.3 * bounds.height()) return false;
    const auto band = [&](int y) {
        double area = 0, moment = 0;
        for (const auto& r : ink.intersected(QRect(bounds.left(), y, bounds.width(), third))) {
            const double pixels = r.width() * r.height();
            area += pixels;
            moment += pixels * (r.left() - bounds.left() + .5 * r.width());
        }
        return std::pair{area, area ? moment / (area * bounds.width()) : 0.};
    };
    const auto [head, headX] = band(bounds.top());
    const auto [tip, tipX] = band(bounds.bottom() - third + 1);
    return headX < .35 && tipX > .8 && tip * 2 < head;
}

int PageAnalysisResult::secondaryBodyContour(ShapeException exception, const QString& text, int markQuota, double baseline,
        double scale, const std::vector<BodyContourInput>& members, const std::function<QRegion(int)>& inkRegion) {
    if (exception == ShapeException::NONE || exception == ShapeException::AlefHamzaJoined ||
        members.size() < 2 || members.front().contours != 1 || !members.front().untyped ||
        !std::isfinite(baseline) || !std::isfinite(scale) || scale <= 0) return -1;
    int contours = 0;
    for (const auto& member : members) contours += member.contours;
    if (contours != markQuota + 2) return -1;

    const bool counter = exception == ShapeException::InnerContourCollidingProblem;
    const auto& primary = members.front().bounds;
    const double reach = scale * 0.5; // half a PDF point, also the native baseline band
    const auto spansBand = [&](const QRectF& b) { return b.top() <= baseline - reach && b.bottom() >= baseline + reach; };
    if (!counter && !spansBand(primary)) return -1;
    const bool lamAlef = !counter && endsInLamAlef(text);
    QRegion body;
    int selected = -1;
    for (size_t i = 1; i < members.size(); ++i) {
        const auto& [box, count, untyped] = members[i];
        if (!untyped || count != 1) continue;
        const bool upperArm = lamAlef && box.bottom() <= baseline &&
            std::abs(box.left() - primary.left()) <= .5 * box.width();
        const double gap = upperArm ? scale : reach;
        if (counter) {
            if (!primary.contains(box) || box.top() > baseline || box.bottom() < baseline) continue;
        } else {
            if (!upperArm && (!spansBand(box) || (box.left() >= primary.left() && box.right() <= primary.right()))) continue;
            if (!primary.adjusted(-gap, -gap, gap, gap).intersects(box)) continue;
        }
        if (body.isEmpty()) body = inkRegion(0);
        const auto candidate = inkRegion(i);
        if (body.isEmpty() || candidate.isEmpty()) continue;
        if (counter) {
            if (candidate == body || !candidate.subtracted(body).isEmpty()) continue;
        } else {
            if (upperArm) {
                if (!isLamAlefArm(candidate)) continue;
                // Rasterization can paint the contact pixel in both source contours.
                const auto contact = body.intersected(candidate).boundingRect();
                if (!contact.isEmpty() && (contact.left() < box.right() - gap || contact.top() < box.bottom() - gap))
                    continue;
            } else if (body.intersects(candidate)) continue;
            bool adjacent = false;
            const int pixels = static_cast<int>(std::floor(gap));
            for (int dy = -pixels; dy <= pixels && !adjacent; ++dy)
                for (int dx = -pixels; dx <= pixels && !adjacent; ++dx)
                    if (dx*dx + dy*dy <= gap*gap)
                        adjacent = body.intersects(candidate.translated(dx, dy));
            if (!adjacent) continue;
        }
        if (selected >= 0) return -1;
        selected = static_cast<int>(i);
    }
    return selected;
}

void PageAnalysisResult::resolveCompoundBody(int pageIndex, int lineIndex, int wordIndex) {
    auto& line = page.lines[lineIndex];
    const auto record = wordShapeException(pageIndex + 1, lineIndex + 1, wordIndex + 1, line.words.size());
    auto& word = line.words[wordIndex];
    if (record.subWordPosition <= 0 || record.subWordPosition > int(word.wordResultInfo.subWords.size())) return;
    auto& sub = word.wordResultInfo.subWords[record.subWordPosition - 1];
    int quota = -1; // one connected base body in the transcript's contour count
    const auto joining = arabic_joining(sub.text);
    for (int i = 0; i < sub.text.size(); ++i) {
        const auto form = joining[i].joining;
        quota += charProps.value(sub.text[i].unicode()).nb_paths[form] - (form == FINA || form == MEDI);
    }
    constexpr double scale = 16.0 / 3.0; // four SVG pixels per unit at 96 / 72 units per PDF point
    const auto toRaster = QTransform::fromScale(scale, scale);
    std::vector<BodyContourInput> members;
    for (const int index : sub.paths) {
        const auto& shape = word.paths[index];
        members.push_back({toRaster.mapRect(boundaryWorldPath(shape).boundingRect()), 1 + shape.nbpath, int(shape.type) == 0});
    }
    const int candidate = secondaryBodyContour(record.shapeException, sub.text, quota, line.baseline * scale, scale, members,
        [&](int i) {
            const auto scaled = toRaster.map(boundaryWorldPath(word.paths[sub.paths[i]]));
            QRegion ink;
            visitSourceInk(scaled, scaled.boundingRect().toAlignedRect().adjusted(-2,-2,2,2),
                [&](int x, int y, int length) { ink += QRect(x, y, length, 1); });
            return ink;
        });
    sub.secondaryBodyPath = candidate >= 0 ? sub.paths[candidate] : -1;
    sub.bodyCounter = candidate >= 0 && record.shapeException == ShapeException::InnerContourCollidingProblem;
}

Apath Word::bodyShape(const WordResultInfo::SubWordInfo& sub) const {
    auto body = paths[sub.paths.front()];
    if (sub.secondaryBodyPath < 0) return body;
    const auto& part = paths[sub.secondaryBodyPath];
    auto secondary = part.path;
    if (part.pos != body.pos || part.transform != body.transform) {
        secondary = boundaryWorldPath(part).translated(-body.pos);
        secondary = body.transform.inverted().map(secondary);
    }
    if (sub.bodyCounter) {
        // Append the original Béziers. A boolean subtraction can flatten the exterior.
        // Choose the inner direction for consumers that paint with nonzero winding.
        const auto bounds = secondary.boundingRect();
        const double scale = 64.0 / std::max(bounds.width(), bounds.height());
        const auto normalized = QTransform::fromScale(scale, scale);
        const auto inner = normalized.map(secondary);
        auto winding = normalized.map(body.path);
        winding.setFillRule(Qt::WindingFill);
        winding.addPath(inner);
        // A colliding edge can leave a sliver even when the opening is clear.
        if (2 * AFont::getArea(winding.intersected(inner)) > AFont::getArea(inner))
            secondary = secondary.toReversed();
        body.path.setFillRule(Qt::OddEvenFill);
    }
    body.path.addPath(secondary);
    return body;
}

WordResultInfo PageAnalysisResult::detectSubWords(int pageIndex, int lineIndex, int wordIndex, AFont* font, bool recompute) {

	auto& line = page.lines[lineIndex];
	auto& word = line.words[wordIndex];

	auto wordText = QuranTextByWord[pageIndex][lineIndex][wordIndex];
	word.text = wordText;
	// Legacy caches preserve their old type flags. Restore missing stop-sign
	// flags with the same geometric matcher as fresh parsing, without changing ink.
	restoreWaqfTypes(pageIndex, lineIndex, wordIndex, font);

	if (word.wordResultInfo.subWords.size() > 0 && !recompute) {
		repairSubwordBoundaries(pageIndex, lineIndex, wordIndex, font);
		resolveCompoundBody(pageIndex, lineIndex, wordIndex);
		return word.wordResultInfo;
	}	

	auto joining = PageAnalysisResult::arabic_joining(wordText);
	std::vector<int> nbContourBySubWords;

	int totalPotentialPaths = 0;
	int subwordIndex = -1;

	word.wordResultInfo = WordResultInfo();
	WordResultInfo& wordInfo = word.wordResultInfo;

	const int pageNumber = pageIndex + 1;
	const auto wordExc = wordShapeException(pageNumber, lineIndex + 1, wordIndex + 1, line.words.size());

	for (int charInd = 0; charInd < wordText.size(); charInd++) {

		auto qchar = wordText[charInd];

		if (!PageAnalysisResult::charProps.contains(qchar.unicode())) {
			throw new std::runtime_error("Unicode not found");
		}

		auto joinType = joining[charInd].joining;

		if (joinType == INIT || joinType == ISOL || (joinType == NONE && !qchar.isMark())) {
			subwordIndex++;
			wordInfo.subWords.push_back({ "" });
			if (wordExc.subWordPosition == (subwordIndex + 1)) {
				if (wordExc.shapeException == ShapeException::AlefHamzaJoined) {
					nbContourBySubWords.push_back(-1);
					totalPotentialPaths--;
				}
				else {
					nbContourBySubWords.push_back(1);
					totalPotentialPaths++;
				}
			}
			else {
				nbContourBySubWords.push_back(0);
			}
		}
		else if (subwordIndex == -1) {
			throw new std::runtime_error("Error");
		}

		auto nbpath = PageAnalysisResult::charProps.value(qchar.unicode()).nb_paths[joining[charInd].joining];
		if (joining[charInd].joining == FINA || joining[charInd].joining == MEDI) {
			--nbpath;
		}



		nbContourBySubWords[subwordIndex] += nbpath;

		wordInfo.subWords[subwordIndex].text += qchar;

		WordResultInfo::CharInfo charInfo;

		//charInfo.startIndex = totalPotentialPaths;

		totalPotentialPaths += nbpath;

		//charInfo.endIndex = totalPotentialPaths;
		charInfo.subWord = subwordIndex;

		wordInfo.charInfos.push_back(charInfo);

	}

	std::vector<std::pair<qreal, int>> rightBR;

	struct TransformedPath {
		QPainterPath path;
		QRectF boundingRect;
		bool intersectBaseLine;
	};

	std::vector<TransformedPath> transformedPaths;

	QRectF baseLineRect{ 0,line.baseline - 0.5,500,1 };


	int shapeIndex = 0;
	int totalRealPaths = 0;
	for (auto shape : word.paths) {
		auto itemPath = shape.path;
		auto pos = shape.pos;
		itemPath = itemPath * shape.transform;
		itemPath.translate(pos.x(), pos.y());
		auto brect = itemPath.boundingRect();

		if (shape.type == CustomDataType::WAQF) {
			brect.setX(0);
			rightBR.push_back({ brect.x(), shapeIndex });


			transformedPaths.push_back({ itemPath ,brect,false });
		}
		else {


			bool intersectBaseLine = itemPath.intersects(baseLineRect);

			rightBR.push_back({ brect.x() + brect.width(), shapeIndex });

			transformedPaths.push_back({ itemPath ,brect,intersectBaseLine });
		}


		shapeIndex++;

		totalRealPaths += 1 + shape.nbpath;
	}

	std::sort(rightBR.begin(), rightBR.end(), [](const std::pair<qreal, int>& a, const std::pair<qreal, int>& b) { return a.first > b.first; });

	//TODO: Exception 
	if (pageIndex == 584 && lineIndex == 11 && wordIndex == 0) {
		std::swap(rightBR[8], rightBR[5]);
		std::swap(rightBR[8], rightBR[6]);
		//std::swap(rightBR[8], rightBR[7]);
	}
	else if (pageIndex == 118 && lineIndex == 1 && wordIndex == 1) {
		std::swap(rightBR[6], rightBR[4]);
		std::swap(rightBR[5], rightBR[4]);
		//std::swap(rightBR[8], rightBR[7]);
	}


	if (totalPotentialPaths != totalRealPaths) {
		std::cout << "totalPotentialPaths=" << totalPotentialPaths << " != totalRealPaths=" << totalRealPaths << " at " << pageIndex + 1 << "-" << lineIndex + 1 << "-" << wordIndex + 1 << std::endl;
		wordInfo.state |= WordResultFlags::DIFFERENT_PATH_NUMBER;
	}

	int indexRealPath = 0;
	for (int subWordIndex = 0; subWordIndex < nbContourBySubWords.size(); subWordIndex++) {
		auto nbContourBySubWord = nbContourBySubWords[subWordIndex];
		for (int i = 0; i < nbContourBySubWord && indexRealPath < rightBR.size(); i++) {
			auto shapeIndex = rightBR[indexRealPath].second;
			auto* shape = &word.paths[shapeIndex];

			int step = indexRealPath + 1;
			while (i + shape->nbpath >= nbContourBySubWord && step < rightBR.size()) {
				auto temp = rightBR[indexRealPath];
				rightBR[indexRealPath] = rightBR[step];
				rightBR[step] = temp;
				shapeIndex = rightBR[indexRealPath].second;
				shape = &word.paths[shapeIndex];
				step++;
				wordInfo.state |= WordResultFlags::NBPATH_OVERFLOW_DETECTED;
			}
			if (i + shape->nbpath >= nbContourBySubWord) {
				//throw new std::runtime_error("Error");
				std::cout << "Error shape->nbpath at " << pageIndex + 1 << "-" << lineIndex + 1 << "-" << wordIndex + 1 << std::endl;
				wordInfo.state |= WordResultFlags::NBPATH_OVERFLOW;
			}

			wordInfo.subWords[subWordIndex].paths.push_back(shapeIndex);

			i += shape->nbpath;
			indexRealPath++;
		}
		// detect case where next subword  inside previous subword (i.e kaf, ain, ...)
		if (subWordIndex + 1 < nbContourBySubWords.size() && indexRealPath < rightBR.size()) {
			auto currentIndex = rightBR[indexRealPath].second;
			auto& currentTransformedPath = transformedPaths[currentIndex];
			auto curect = currentTransformedPath.boundingRect;

			QRectF wordRect;
			qreal farX = 1000;
			int lastIndex = -1;
			int lastShapeIndex = -1;

			for (int itemI = 0; itemI < wordInfo.subWords[subWordIndex].paths.size(); itemI++) {
				auto shapeIndex = wordInfo.subWords[subWordIndex].paths[itemI];
				auto brect = transformedPaths[shapeIndex].boundingRect;
				if (brect.x() < farX) {
					farX = brect.x();
					lastIndex = itemI;
					lastShapeIndex = shapeIndex;
				}
				wordRect = wordRect.united(brect);
			}
			auto& last = transformedPaths[lastShapeIndex];
			auto brect = last.boundingRect;
			auto diff = curect.x() - brect.x();
			if (pageIndex == 584 && lineIndex == 11 && wordIndex == 0 && subWordIndex == 2) {
			}
			else if (pageIndex == 118 && lineIndex == 1 && wordIndex == 1 && subWordIndex == 1) {
			}
			else if (curect.bottom() < brect.top() && last.intersectBaseLine && nbContourBySubWord == 1) {
			}
			else if (diff > 5 || (word.paths[lastShapeIndex].type == CustomDataType::WAQF)) {
				auto& paths = wordInfo.subWords[subWordIndex].paths;
				paths.erase(paths.begin() + lastIndex);
				paths.push_back(currentIndex);
				std::swap(rightBR[indexRealPath - (paths.size() - lastIndex)], rightBR[indexRealPath]);
				wordInfo.state |= WordResultFlags::SUBWORD_OVERFLOW;

			}
			else {

				auto currentOverlapping = curect.right() - brect.left();
				auto newOverlapping = brect.right() - curect.left();
				if (newOverlapping < currentOverlapping || (nbContourBySubWord == 1 && (brect.bottom() < curect.top() || brect.top() > curect.bottom()) && currentTransformedPath.intersectBaseLine)) { // || (nbContourBySubWord == 1 && !last.intersectBaseLine && currentTransformedPath.intersectBaseLine)
					auto& paths = wordInfo.subWords[subWordIndex].paths;
					paths.erase(paths.begin() + lastIndex);
					paths.push_back(currentIndex);
					std::swap(rightBR[indexRealPath - (paths.size() - lastIndex)], rightBR[indexRealPath]);
					wordInfo.state |= WordResultFlags::SUBWORD_OVERFLOW;
				}
			}
		}
		//adjust base glyph
		//TODO: ALEF HAMZA detection
		auto& paths = wordInfo.subWords[subWordIndex].paths;
		qreal maxValue = 0;
		int bestBasePath = -1;
		qreal maxValueIntersect = 0;
		int bestBasePathIntersect = -1;
		for (int index = 0; index < paths.size(); index++) {
			auto pathIndex = paths[index];
			if ((int)word.paths[pathIndex].type > 0) continue;
			auto& tranPath = transformedPaths[pathIndex];
			auto value = tranPath.boundingRect.height();
			if (tranPath.intersectBaseLine && value > maxValueIntersect) {
				maxValueIntersect = value;
				bestBasePathIntersect = index;
			}
			if (value > maxValue) {
				maxValue = value;
				bestBasePath = index;
			}
		}
		if (bestBasePathIntersect >= 0) {
			std::swap(paths[0], paths[bestBasePathIntersect]);
		}
		else if (bestBasePath >= 0) {
			wordInfo.state |= WordResultFlags::NO_INTERSECT_BASELINE;
			std::swap(paths[0], paths[bestBasePath]);
		}

		auto text = wordInfo.subWords[subWordIndex].text;

		if ((pageIndex == 388 && lineIndex == 2 && wordIndex == 3 && subWordIndex == 2)
			|| (pageIndex == 84 && lineIndex == 4 && wordIndex == 1 && subWordIndex == 2)
			|| (pageIndex == 584 && lineIndex == 6 && wordIndex == 6 && subWordIndex == 2)
			|| (pageIndex == 121 && lineIndex == 9 && wordIndex == 2 && subWordIndex == 1)
			) {
			auto& tranPath0 = word.paths[paths[0]];
			auto& tranPath1 = word.paths[paths[1]];
			if (font->checkGlyph(tranPath0.path, QChar(0x064E), constants::MATCHING_ERROR_RATIO) != -1) {
				if (font->checkGlyph(tranPath1.path, QChar(0x064E), constants::MATCHING_ERROR_RATIO) != -1) {
					//std::cout << "Found" << std::endl;
					std::swap(paths[2], paths[0]);
					std::swap(paths[2], paths[1]);
				}
				else if (font->checkGlyph(tranPath1.path, "twodots", CompareMethod::AREA, 0.15) != -1) {
					//std::cout << "Found" << std::endl;
					std::swap(paths[2], paths[0]);
					std::swap(paths[2], paths[1]);
				}
				else if (font->checkGlyph(tranPath1.path, QChar(0x064E), constants::MATCHING_ERROR_RATIO) == -1) {
					//std::cout << "Found" << std::endl;
					std::swap(paths[0], paths[1]);
				}
			}
			else if (font->checkGlyph(tranPath0.path, "twodots", CompareMethod::AREA, 0.15) != -1) {
				//std::cout << "Found" << std::endl;
				std::swap(paths[0], paths[1]);
			}
		}
		else if (text.size() <= 3) {
			if (text[1].unicode() == 0x0650 || text[1].unicode() == 0x064F) {
				auto& tranPath = word.paths[paths[0]];
				if (font->checkGlyph(tranPath.path, text[1], constants::MATCHING_ERROR_RATIO) != -1) {
					//std::cout << "Found" << std::endl;
					std::swap(paths[0], paths[1]);
				}
			}
		}


	}

	//analyzeSubwords(pageIndex, lineIndex, wordIndex, wordInfo, font);
	repairSubwordBoundaries(pageIndex, lineIndex, wordIndex, font);
	resolveCompoundBody(pageIndex, lineIndex, wordIndex);

	return wordInfo;

}

void PageAnalysisResult::analyzeSubwords(int pageIndex, int lineIndex, int wordIndex, WordResultInfo& wordResultInfo, AFont* font) {

	auto& line = page.lines[lineIndex];
	auto& word = line.words[wordIndex];

	for (int subWordIndex = 0; subWordIndex < wordResultInfo.subWords.size(); subWordIndex++) {
		auto& subWord = wordResultInfo.subWords[subWordIndex];
		if (subWord.text.contains("إ")) {
			for (int shapeIndex = 0; shapeIndex < subWord.paths.size(); shapeIndex++) {
				auto shapePathIndex = subWord.paths[shapeIndex];
				auto& shape = word.paths[shapePathIndex];
				double value;
				if (font->checkGlyph(shape.path, "alef", CompareMethod::TF, 0.2, &value) != -1) {
					if (shapeIndex != 0) {
						rotateVector(subWord.paths, shapeIndex, 0);
						std::cout << "Alef found, minValue=" << value << ",Index=" << shapeIndex << ",at " << pageIndex + 1 << "-" << lineIndex + 1 << "-" << wordIndex + 1 << std::endl;
					}
					break;
				}
			}


		}

	}
}
int PageAnalysisResult::loadPage(int pageNumber, AFont* font, bool debug, QGraphicsScene* extScene) {

	QGraphicsScene localscene;

	QGraphicsScene* scene = extScene != nullptr ? extScene : &localscene;

	LinesInfo.clear();
	page.lines.clear();
	subwordBoundaryRepairs=QJsonArray();
	waqfTypeRepairs=QJsonArray();
	headingPaths.clear();

	QString fileName = "./input/1441-AI-hafs/";
	char str[4];
	snprintf(str, 4, "%03d", pageNumber);
	fileName = fileName + str + "___Hafs39__DM.ai";

	QFile file(fileName);
	if (!file.open(QFile::ReadOnly | QFile::Text)) {
		//QMessageBox::warning(this, tr("Application"),
		//	tr("Cannot read file %1:\n%2.")
		//	.arg(QDir::toNativeSeparators(fileName), file.errorString()));
		return -1;
	}

	analyzeTextPage(pageNumber);

	notMatchedItems.clear();


	auto filename_g = std::make_unique<GooString>(fileName.toStdString()); 

	PDFDoc doc(std::move(filename_g));

	//SplashColor sc = { 255, 255, 255 };

//	globalParams = std::make_unique<GlobalParams>();


	CustomOutputDev output{};
	output.startDoc(&doc);
	doc.displayPageSlice(&output, 1, 72, 72, 0, false, true, false, -1, -1, -1, -1);

	auto& paths = output.getPaths();

	scene->clear();

	scene->setSceneRect(0, 0, 400, 520);

	//std::cout << "**************************PAGE=" << pageNumber << std::endl;	

	for (auto& path : paths) {
		auto color = path.brush.color();
		if (color.red() == 35) {
			auto item = new QGraphicsPathItem(path.path);
			item->setFlag(QGraphicsItem::ItemIsSelectable, true);
			item->setFlag(QGraphicsItem::ItemIsMovable, true);
			item->setTransform(path.transform);
			item->setBrush(path.brush);
			item->setPos(path.pos);
			item->setPen(Qt::NoPen);
			auto bbox = item->boundingRect();
			//if (bbox.width() < 0.05 && bbox.height() < 0.05) {
			//if (bbox.width() < 0.2 && bbox.height() < 0.2) {
			if (bbox.width() < 0.2 || bbox.height() < 0.2) {
				//std::cout << "Object very tiny at page " << pageNumber  << std::endl;
				delete item;
			}
			else {
				scene->addItem(item);
			}
		}
	}

	QSet<QGraphicsPathItem*> removedItems;
	/*
	auto containsBar = pageNumber == 341 || pageNumber == 334 || pageNumber == 598 || pageNumber == 416 || pageNumber == 379
		|| pageNumber == 528 || pageNumber == 454 || pageNumber == 309 || pageNumber == 272 || pageNumber == 251 || pageNumber == 176 || pageNumber == 365
		|| pageNumber == 293 || pageNumber == 589 || pageNumber == 480;*/

	for (auto& item : scene->items()) {

		QGraphicsPathItem* pathItem = (QGraphicsPathItem*)item;

		if (removedItems.contains(pathItem)) continue;

		//check fathatan, kasratan, fathatanidgham, kasratanidgham
		checkGlyph(pathItem, font, scene, removedItems);



		//Sajda bar
		auto bbox = pathItem->boundingRect();

		if (bbox.width() > 15 && bbox.height() < 0.7) {
			removedItems.insert(pathItem);
		}

	}

	for (auto& item : removedItems) {
		scene->removeItem(item);
	}

	if (pageNumber % 2 == 0) {
		left = leftEvent;
	}
	else {
		left = leftOdd;
	}

	scene->setSceneRect(left, firstliney - 30, lineWidth, textHeight + 40);

	if (debug) {
		debugLines(pageNumber, font, scene);
	}
	else {
		detectWords(pageNumber, font, scene);
		for (int lineIndex = 0; lineIndex < page.lines.length(); lineIndex++) {
			auto& line = page.lines[lineIndex];
			for (int wordIndex = 0; wordIndex < line.words.length(); wordIndex++) {
				detectSubWords(pageNumber - 1, lineIndex, wordIndex, font);
			}
		}
		resolveWordBoundaries(pageNumber, font);
	}

	return 0;
}



const arabic_state_table_entry PageAnalysisResult::arabic_state_table[3][4] =
{
	/*   jt_U,          jt_L,          jt_R,          jt_D,          */

	/* State 0: prev was U, not willing to join. */
	{ {NONE,NONE,0}, {NONE,ISOL,1}, {NONE,ISOL,0}, {NONE,ISOL,1} },

	/* State 1: prev was D/L in ISOL form, willing to join. */
	{ {NONE,NONE,0}, {NONE,ISOL,1}, {INIT,FINA,0}, {INIT,FINA,2} },

	/* State 2: prev was D in FINA form, willing to join. */
	{ {NONE,NONE,0}, {NONE,ISOL,1}, {MEDI,FINA,0}, {MEDI,FINA,2}}
};




const unsigned int PageAnalysisResult::joining_mapping[6] = { 0,3,3,2,1,5 };

QVector<TextInfo> PageAnalysisResult::arabic_joining(QString text) {
	QVector<TextInfo> info;
	unsigned int prev = UINT_MAX, state = 0;

	for (unsigned int i = 0; i < text.size(); i++)
	{

		info.append(TextInfo{});
		auto qchar = text[i];

		auto qjoining = qchar.joiningType();


		if (qjoining == QChar::Joining_Transparent) {
			info[i].joining = NONE;
			continue;
		}

		const arabic_state_table_entry* entry = &PageAnalysisResult::arabic_state_table[state][joining_mapping[qjoining]];

		if (entry->prev_action != NONE && prev != UINT_MAX)
		{
			info[prev].joining = entry->prev_action;

		}

		info[i].joining = entry->curr_action;

		prev = i;
		state = entry->next_state;
	}

	return info;
}


static QMap<ushort, UnicodeProp> initUnicodProp() {
	QMap<ushort, UnicodeProp> charProps;

	charProps[0X0000] = { 0 }; // notdef
	charProps[0X0001] = { 0 }; // null
	charProps[0X000A] = { 0 }; // linefeed
	charProps[0X200D] = { 0 }; // zwj
	charProps[0X034F] = { 0 }; // cgj
	charProps[0X0020] = { 0 }; // space
	charProps[0X06DE] = { 8 }; // rubelhizb
	charProps[0X06E9] = { 2 }; // placeofsajdah
	charProps[0X06DD] = { 0 }; // endofaya
	charProps[0X064E] = { 1 }; // fatha
	charProps[0X0650] = { 1 }; // kasra
	charProps[0X064B] = { 2 }; // fathatan
	charProps[0X064D] = { 2 }; // kasratan
	charProps[0X08F0] = { 2 }; // fathatanidgham
	charProps[0X08F2] = { 2 }; // kasratanidgham
	charProps[0X064F] = { 1 }; // damma
	charProps[0X08F1] = { 2 }; // dammatanidgham
	charProps[0X064C] = { 2 }; // dammatan
	charProps[0X0651] = { 1 }; // shadda
	charProps[0X06E1] = { 1 }; // headkhah
	charProps[0X0652] = { 1 }; // sukun
	charProps[0X0653] = { 1 }; // maddahabove
	charProps[0X0670] = { 1 }; // smallalef
	charProps[0X06E2] = { 1 }; // meemiqlab
	charProps[0X06ED] = { 1 }; // smalllowmeem
	charProps[0X06E5] = { 1 }; // smallwaw
	charProps[0X06E6] = { 1 }; // smallyeh
	charProps[0X06E7] = { 1 }; // smallhighyeh
	charProps[0X08F3] = { 1 }; // smallhighwaw
	charProps[0X06DF] = { 1 }; // smallhighroundedzero
	charProps[0X06E0] = { 1 }; // rectangularzero
	charProps[0X06EC] = { 1 }; // roundedfilledhigh
	charProps[0X065C] = { 1 }; // roundedfilledlow
	charProps[0X0654] = { 1 }; // hamzaabove
	charProps[0X0655] = { 1 }; // hamzabelow
	charProps[0X06D9] = { 1 }; // waqf.lam
	charProps[0X06D7] = { 2 }; // waqf.qaf
	charProps[0X06DA] = { 2 }; // waqf.jeem
	charProps[0X06D6] = { 1 }; // waqf.sad
	charProps[0X06DB] = { 3 }; // waqf.smallhighthreedots
	charProps[0X06D8] = { 1 }; // waqf.meem
	charProps[0X06DC] = { 1 }; // smallhighseen
	charProps[0X06E3] = { 1 }; // smalllowseen
	charProps[0X06E8] = { 2 }; // smallhighnoon
	charProps[0X0621] = { 1 }; // hamza.isol
	charProps[0X0627] = { 1 }; // alef.isol
	charProps[0X0633] = { 1 }; // seen.isol
	charProps[0X0644] = { 1 }; // lam.isol
	charProps[0X0645] = { 1 }; // meem.isol
	charProps[0X0646] = { 2 }; // noon.isol
	charProps[0X0642] = { 2 }; // qaf.isol
	charProps[0X0641] = { 2 }; // feh.isol
	charProps[0X062D] = { 1 }; // hah.isol
	charProps[0X062F] = { 1 }; // dal.isol
	charProps[0X0631] = { 1 }; // reh.isol
	charProps[0X0648] = { 1 }; // waw.isol
	charProps[0X0647] = { 1 }; // heh.isol
	charProps[0X0635] = { 1 }; // sad.isol
	charProps[0X0639] = { 1 }; // ain.isol
	charProps[0X0637] = { 1 }; // tah.isol
	charProps[0X0649] = { 1 }; // alefmaksura.isol
	charProps[0X064A] = { 1,2,2,1 }; // yehshape.isol
	charProps[0X0643] = { 2,1,1,2 }; // kaf.isol
	charProps[0X0660] = { 1 }; // zeroindic
	charProps[0X0661] = { 1 }; // oneindic
	charProps[0X0662] = { 1 }; // twoindic
	charProps[0X0663] = { 1 }; // threeindic
	charProps[0X0664] = { 1 }; // fourindic
	charProps[0X0665] = { 1 }; // fiveindic
	charProps[0X0666] = { 1 }; // sixindic
	charProps[0X0667] = { 1 }; // sevenindic
	charProps[0X0668] = { 1 }; // eightindic
	charProps[0X0669] = { 1 }; // nineindic
	charProps[0X0640] = { 1 }; // tatweel

	//not declared

	charProps[0x0622] = { 2 }; // alef.maddahabove.isol
	charProps[0x0623] = { 2 }; // alef.hamzaabove.isol
	charProps[0x0624] = { 2 }; // waw.hamzaabove.isol"
	charProps[0x0625] = { 2 }; // alef.hamzabelow.isol
	charProps[0x0626] = { 2 }; // alefmaksura.hamzaabove.isol
	charProps[0x0628] = { 2 }; // behshape.onedotdown.isol
	charProps[0x0629] = { 2 }; // heh.twodotsup.isol
	charProps[0x062A] = { 2 }; // behshape.twodotsup.isol
	charProps[0x062B] = { 3 }; // behshape.three_dots.isol	
	charProps[0x062C] = { 2 }; // hah.onedotdown.isol
	charProps[0x062E] = { 2 }; // hah.onedotup.isol
	charProps[0x0630] = { 2 }; // dal.onedotup.isol
	charProps[0x0632] = { 2 }; // reh.onedotup.isol
	charProps[0x0634] = { 3 }; // seen.three_dots.isol
	charProps[0x0636] = { 2 }; // sad.onedotup.isol
	charProps[0x0638] = { 2 }; // tah.onedotup.isol
	charProps[0x063A] = { 2 }; // ain.onedotup.isol
	charProps[0x0671] = { 2 }; // alef.wasla.isol

	return charProps;
}

QMap<ushort, UnicodeProp> PageAnalysisResult::charProps = initUnicodProp();

void PageAnalysisResult::analyzeTextPage(int pageNumber) {

	auto textLines = quranText[pageNumber - 1];

	QRegularExpression re(surapattern, QRegularExpression::MultilineOption);

	for (int i = 0; i < textLines.size(); i++) {

		LinesInfo.append(LineInfo{});

		LinesInfo[i].text = textLines[i];

		auto match = re.match(LinesInfo[i].text);

		LinesInfo[i].isSura = match.hasMatch();


		auto& textLine = LinesInfo[i].text;

		auto words = textLine.split(char(0x20), Qt::SkipEmptyParts);
		for (int wordIndex = 0; wordIndex < words.size(); wordIndex++) {
			int wordNb = wordIndex + 1;
			int wordnbpath = 0;
			auto word = words[wordIndex];
			auto info = arabic_joining(word);
			for (int charInd = 0; charInd < word.size(); charInd++) {

				auto qchar = word[charInd];
				if (charProps.contains(qchar.unicode())) {
					auto nbpath = charProps.value(qchar.unicode()).nb_paths[info[charInd].joining];
					if (info[charInd].joining == FINA || info[charInd].joining == MEDI) {
						--nbpath;
					}

					wordnbpath += nbpath;

				}
				else {
					throw new std::runtime_error("Unicode not found");
				}
			}
			const auto exception = wordShapeException(pageNumber, i + 1, wordNb, words.size()).shapeException;
			if (exception != ShapeException::NONE)
				wordnbpath += exception == ShapeException::AlefHamzaJoined ? -1 : 1;
			LinesInfo[i].wordInfos.append({ wordnbpath });
		}
	}

}

void PageAnalysisResult::analyzePage(int pageNumber, AFont* font, QGraphicsScene* scene, QVector<QSet<QGraphicsPathItem*>>& lines) {

	int totalTextPathCount = 0;
	int totalShapePathCount = 0;

	for (int i = 0; i < lines.size(); i++) {

		if (LinesInfo[i].isSura) continue;

		auto& line = lines[i];

		int lineTextPathCount = 0;
		int lineShapePathCount = 0;

		for (auto pathItem : line) {
			lineShapePathCount += 1 + pathItem->data((int)CustomData::NBPATH).toInt();
		}

		auto& lineResult = LinesInfo[i].wordInfos;
		for (auto wordResult : lineResult) {
			lineTextPathCount += wordResult.nbContoursByByWord;
		}

		totalTextPathCount += lineTextPathCount;
		totalShapePathCount += lineShapePathCount;

		int difference = lineShapePathCount - lineTextPathCount;

		if (difference != 0) {
			std::cout << "Page " << pageNumber << ", Line " << (i + 1) << ", Nb Chars = " << LinesInfo[i].text.size() << ", textPathCount = " << lineTextPathCount << ", Nb PathItem = " << line.size() << ", shapePathCount = " << lineShapePathCount << std::endl;

			if (line.size() > 0) {
				QMultiMap<int, QGraphicsPathItem*> maps;

				for (auto pathItem : line) {
					auto brect = pathItem->sceneBoundingRect();
					maps.insert(brect.x() + brect.width(), pathItem);

				}

				int itemIndex = 1;
				QFont myFont;

				QTransform tranform{ 1,0,0,-1,0,0 };

				myFont.setPointSizeF(1);
				auto i = maps.end();
				do {
					i--;
					auto item = i.value();
					auto brect = item->boundingRect();

					QPainterPath textPath;

					textPath.addText(brect.x(), -brect.y(), myFont, QString("%1").arg(itemIndex));

					textPath = textPath * tranform;

					//std::cout << itemIndex << ":x=" << brect.x() << ",y=" << brect.y() << ",width=" << brect.width() << ",height=" << brect.height() << ",shapeadded=" << item->data((int)CustomData::NBPATH).toInt() << std::endl;
					auto path = item->path();
					//path.addText(brect.x(), brect.y(), myFont, QString("%1").arg(itemIndex));
					path.addPath(textPath);
					item->setPath(path);

					itemIndex++;

				} while (i != maps.begin());
			}
		}

	}

	if (totalTextPathCount != totalShapePathCount) {
		std::cout << "Page " << pageNumber << ", totalTextPathCount = " << totalTextPathCount << ", totalShapePathCount = " << totalShapePathCount << std::endl;
	}
}
