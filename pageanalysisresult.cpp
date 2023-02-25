#include "pageanalysisresult.h"
#include "CustomOutputDev.h"
#include "PDFDoc.h"
#include "GlobalParams.h"
#include "afont.h"
#include "quran.h"
#include <format>
#include "qfile.h"


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
	int pageNumber = 0;
	for (auto textLines : PageAnalysisResult::quranText) {
		pageNumber++;
		QVector<QStringList> page;
		if (pageNumber < 3) {
			QuranTextByWord.append(page);
			continue;
		}

		for (int i = 0; i < textLines.size(); i++) {
			auto textLine = textLines[i];
			auto match = re.match(textLine);
			if (match.hasMatch()) {
				page.append({});
				continue;
			}
			auto words = textLine.split(char(0x20), Qt::SkipEmptyParts);
			page.append(words);
		}
		QuranTextByWord.append(page);

	}
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
	else if (font->checkGlyph(itemPath, "sad.waqf") != -1) {
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

WordResultInfo PageAnalysisResult::detectSubWords(int pageIndex, int lineIndex, int wordIndex, AFont* font, bool recompute) {

	auto& line = page.lines[lineIndex];
	auto& word = line.words[wordIndex];

	if (word.wordResultInfo.subWords.size() > 0 && !recompute) {
		return word.wordResultInfo;
	}

	auto wordText = QuranTextByWord[pageIndex][lineIndex][wordIndex];

	auto joining = PageAnalysisResult::arabic_joining(wordText);
	std::vector<int> nbContourBySubWords;

	int totalPotentialPaths = 0;
	int subwordIndex = -1;

	word.wordResultInfo = WordResultInfo();
	WordResultInfo& wordInfo = word.wordResultInfo;

	ShapeExceptionRecord wordExc{};
	int pageNumber = pageIndex + 1;
	if (nbShapeExceptions.contains(pageNumber)) {
		auto pageExce = nbShapeExceptions.value(pageNumber);
		if (pageExce.contains(lineIndex + 1)) {
			auto lineExce = pageExce.value(lineIndex + 1);

			if (lineExce.contains(wordIndex + 1)) {
				wordExc = lineExce.value(wordIndex + 1);
			}
			else if (lineExce.contains(wordIndex - line.words.size())) {
				wordExc = lineExce.value(wordIndex - line.words.size());
			}
		}
	}

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
			auto diff2 = brect.right() - curect.right();
			if (pageIndex == 584 && lineIndex == 11 && wordIndex == 0 && subWordIndex == 2) {
			}
			else if (pageIndex == 118 && lineIndex == 1 && wordIndex == 1 && subWordIndex == 1) {
			}
			else if (curect.bottom() < brect.top() && last.intersectBaseLine && nbContourBySubWord == 1) {
			}
			else if (diff > 5 || (word.paths[lastShapeIndex].type == CustomDataType::WAQF)) { //|| (diff > 0 && !last.intersectBaseLine && diff > diff2)
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

	analyzeSubwords(pageIndex, lineIndex, wordIndex, wordInfo, font);

	return wordInfo;

}

void PageAnalysisResult::analyzeSubwords(int pageIndex, int lineIndex, int wordIndex, WordResultInfo& wordResultInfo, AFont* font) {

	auto& line = page.lines[lineIndex];
	auto& word = line.words[wordIndex];

	for (int subWordIndex = 0; subWordIndex < wordResultInfo.subWords.size(); subWordIndex++) {
		auto& subWord = wordResultInfo.subWords[subWordIndex];
		if (subWord.text.contains("إ")) {
			bool found = false;
			double minValue = MAXINT;
			double minIndex = -1;
			for (int shapeIndex = 0; shapeIndex < subWord.paths.size(); shapeIndex++) {
				auto shapePathIndex = subWord.paths[shapeIndex];
				auto& shape = word.paths[shapePathIndex];
				double value;				
				if (font->checkGlyph(shape.path, "alef", CompareMethod::TF, 0.2, &value) != -1) {
					found = true;
					if (shapeIndex != 0) {
						rotateVector(subWord.paths, shapeIndex, 0);
						std::cout << "Alef found, minValue=" << value << ",Index=" << shapeIndex << ",at " << pageIndex + 1 << "-" << lineIndex + 1 << "-" << wordIndex + 1 << std::endl;
					}					
					break;
				}
				else if (value < minValue) {
					minValue = value;
					minIndex = shapeIndex;
				}
			}
			if (!found) {
				//std::cout << "Alef not found, minValue=" << minValue << ",Index=" << minIndex << ",at " << pageIndex + 1 << "-" << lineIndex + 1 << "-" << wordIndex + 1 << std::endl;
			}


		}

	}
}
int PageAnalysisResult::loadPage(int pageNumber, AFont* font, bool debug, QGraphicsScene* extScene) {

	QGraphicsScene localscene;

	QGraphicsScene* scene = extScene != nullptr ? extScene : &localscene;

	LinesInfo.clear();
	page.lines.clear();

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


	auto filename_g = new GooString(fileName.toStdString());

	PDFDoc doc(filename_g, nullptr, nullptr);

	//SplashColor sc = { 255, 255, 255 };

//	globalParams = std::make_unique<GlobalParams>();


	CustomOutputDev output{};
	output.startDoc(&doc);
	doc.displayPageSlice(&output, 1, 72, 72, 0, false, true, false, -1, -1, -1, -1);

	auto& paths = output.getPaths();

	scene->clear();

	scene->setSceneRect(0, 0, 400, 520);

	//std::cout << "**************************PAGE=" << pageNumber << std::endl;	

	int i = -1;
	for (auto& path : paths) {
		//if (path->brush() == QBrush(Qt::SolidPattern)) {
		auto color = path.brush.color();
		auto red = color.red();
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

		info.append({});
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

	const char* JoiningTypes[] =
	{
		"Joining_None",
		"Joining_Causing",
		"Joining_Dual",
		"Joining_Right",
		"Joining_Left",
		"Joining_Transparent"
	};

	for (int i = 0; i < textLines.size(); i++) {

		LinesInfo.append({});

		LinesInfo[i].text = textLines[i];

		auto match = re.match(LinesInfo[i].text);

		LinesInfo[i].isSura = match.hasMatch();


		auto& textLine = LinesInfo[i].text;

		int lineTextPathCount = 0;

		auto words = textLine.split(char(0x20), Qt::SkipEmptyParts);
		QVector<WordInfo> wordInfos;
		for (int wordIndex = 0; wordIndex < words.size(); wordIndex++) {
			int wordNb = wordIndex + 1;
			int wordnbpath = 0;
			auto word = words[wordIndex];
			auto info = arabic_joining(word);
			for (int charInd = 0; charInd < word.size(); charInd++) {

				auto qchar = word[charInd];
				if (charProps.contains(qchar.unicode())) {
					auto test = charProps.value(qchar.unicode());

					auto nbpath = charProps.value(qchar.unicode()).nb_paths[info[charInd].joining];
					if (info[charInd].joining == FINA || info[charInd].joining == MEDI) {
						--nbpath;
					}

					wordnbpath += nbpath;

					//auto joining = info[charInd].joining == NONE ? "NONE" : info[charInd].joining == ISOL ? "ISOL" : info[charInd].joining == FINA ? "FINA" : info[charInd].joining == MEDI ? "MEDI" : "INIT";
					//std::cout << std::format("U+{:04X}({:s}-{:s}-{:s}, total={:d}, nbpath={:d})", qchar.unicode(), JoiningTypes[qchar.joiningType()], qchar.isMark() ? "Mark" : "Base", joining, textPathCount, nbpath) << std::endl;
				}
				else {
					throw new std::runtime_error("Unicode not found");
				}
			}
			if (nbShapeExceptions.contains(pageNumber)) {
				auto pageExce = nbShapeExceptions.value(pageNumber);
				if (pageExce.contains(i + 1)) {
					auto lineExce = pageExce.value(i + 1);
					ShapeExceptionRecord wordExc{};
					if (lineExce.contains(wordNb)) {
						wordExc = lineExce.value(wordNb);
					}
					else if (lineExce.contains(wordIndex - words.size())) {
						wordExc = lineExce.value(wordIndex - words.size());
					}
					if (wordExc.shapeException != ShapeException::NONE) {
						if (wordExc.shapeException == ShapeException::AlefHamzaJoined) {
							wordnbpath--;
						}
						else {
							wordnbpath++;
						}
					}

				}
			}
			LinesInfo[i].wordInfos.append({ wordnbpath });
			lineTextPathCount += wordnbpath;
			//std::cout << "WORD " << wordNb << " , wordnbpath=" << wordnbpath << ", lineTextPathCount = " << lineTextPathCount << std::endl;
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