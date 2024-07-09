#include "quransearch.h"

QuranSearch::QuranSearch() {

	initRexpClasses();

	PageAnalysisResult::initQuranText();
}

void QuranSearch::initRexpClasses() {


	charClusters = {
		{u'ا',{"[أإٱآا]","[اأإٱآ]","[أإٱآا]","[اأإٱآ]"}},
		{u'ب',{"[بتث]","[بتثنيىئ]","[بتثنيىئ]","[بتث]"}},
		{u'ت',{"[بتث]","[بتثنيىئ]","[بتثنيىئ]","[بتث]"}},
		{u'ث',{"[بتث]","[بتثنيىئ]","[بتثنيىئ]","[بتث]"}},
		{u'ج',{"[جحخ]","[جحخ]","[جحخ]","[جحخ]"}},
		{u'ح',{"[جحخ]","[جحخ]","[جحخ]","[جحخ]"}},
		{u'خ',{"[جحخ]","[جحخ]","[جحخ]","[جحخ]"}},
		{u'د',{"[دذ]","[دذ]","[دذ]","[دذ]"}},
		{u'ذ',{"[دذ]","[دذ]","[دذ]","[دذ]"}},
		{u'ر',{"[رز]","[رز]","[رز]","[رز]"}},
		{u'ز',{"[رز]","[رز]","[رز]","[رز]"}},
		{u'س',{"[سش]","[سش]","[سش]","[سش]"}},
		{u'ش',{"[سش]","[سش]","[سش]","[سش]"}},
		{u'ص',{"[صض]","[صض]","[صض]","[صض]"}},
		{u'ض',{"[صض]","[صض]","[صض]","[صض]"}},
		{u'ط',{"[طظ]","[طظ]","[طظ]","[طظ]"}},
		{u'ظ',{"[طظ]","[طظ]","[طظ]","[طظ]"}},
		{u'ع',{"[عغ]","[عغ]","[عغ]","[عغ]"}},
		{u'غ',{"[عغ]","[عغ]","[عغ]","[عغ]"}},
		{u'ف',{"[ف]","[فق]","[فق]","[ف]"}},
		{u'ق',{"[ق]","[فق]","[فق]","[ق]"}},
		{u'ن',{"[ن]","[بتثنيىئ]","[بتثنيىئ]","[ن]"}},
		{u'ه',{"[هة]","[هة]","[هة]","[هة]"}},
		{u'ة',{"[هة]","[هة]","[هة]","[هة]"}},
		{u'ي',{"[يئى]","[بتثنيىئ]","[بتثنيىئ]","[يئى]"}},
		{u'ئ',{"[يئى]","[بتثنيىئ]","[بتثنيىئ]","[يئى]"}},
		{u'ق',{"[ق]","[فق]","[فق]","[ق]"}},
		{u'و',{"[وؤ]","[وؤ]","[وؤ]","[وؤ]"}},
	};

	for (int i = 0x600; i < 0x6FF; i++) {
		QChar qchar{ i };

		auto joiningType = qchar.joiningType();

		if (qchar.isMark() || qchar.isNumber()) continue;

		if (!PageAnalysisResult::charProps.contains(i)) continue;

		if (joiningType == QChar::Joining_Dual) {
			rExpClasses["Dual"].append(qchar);
			rExpClasses["DualRight"].append(qchar);
		}
		else if (joiningType == QChar::Joining_Right) {
			rExpClasses["Right"].append(qchar);
			rExpClasses["DualRight"].append(qchar);
			rExpClasses["NoneRight"].append(qchar);
		}
		else if (joiningType == QChar::Joining_None) {
			rExpClasses["None"].append(qchar);
			rExpClasses["NoneRight"].append(qchar);
		}
	}

	for (auto key : rExpClasses.keys()) {
		rExpClasses[key] = "[" + rExpClasses.value(key) + "]";
	}
}

std::vector<WordMatch> QuranSearch::searchText(QString textToSearch, bool checkBoxMarks, bool checkBoxSubWords,
	bool checkBoxShapes, bool checkBoxIsol, bool checkBoxInit, bool checkBoxMedi, bool checkBoxFina) {

	std::vector<WordMatch> matches;


	QChar firstBase;
	QChar lastBase;
	int totalBase = 0;

	for (int i = 0; i < textToSearch.length(); i++) {
		if (!textToSearch[i].isMark()) {
			if (firstBase.isNull()) {
				firstBase = textToSearch[i];
			}
			lastBase = textToSearch[i];
			totalBase++;
		}
	}


	auto getPattern = [this, checkBoxMarks, checkBoxSubWords, checkBoxShapes, checkBoxIsol, checkBoxInit, checkBoxMedi, checkBoxFina, &textToSearch, firstBase, lastBase, totalBase](int type) -> QString {

		auto markIncluded = checkBoxMarks;
		auto& markClass = this->markClass;

		if (checkBoxSubWords) {
			if (type == 1 && (firstBase.joiningType() != QChar::Joining_Dual || lastBase.joiningType() != QChar::Joining_Dual)) {
				return QString();
			}
			if (type == 2 && (firstBase.joiningType() != QChar::Joining_Dual || (lastBase.joiningType() != QChar::Joining_Dual))) {
				return QString();
			}
			else if (type == 3 && ((lastBase.joiningType() != QChar::Joining_Right && lastBase.joiningType() != QChar::Joining_Dual) || (totalBase > 1 && firstBase.joiningType() != QChar::Joining_Dual))) {
				return QString();
			}
		}

		QVector<TextInfo> joiningResult;
		if (type == 0) {
			joiningResult = PageAnalysisResult::arabic_joining(textToSearch);
		}
		else if (type == 1) {
			joiningResult = PageAnalysisResult::arabic_joining(textToSearch + QString(u'ب'));
			joiningResult.removeLast();
		}
		else if (type == 2) {
			joiningResult = PageAnalysisResult::arabic_joining(QString(u'ب') + textToSearch + QString(u'ب'));
			joiningResult.removeFirst();
			joiningResult.removeLast();
		}
		else if (type == 3) {
			joiningResult = PageAnalysisResult::arabic_joining(QString(u'ب') + textToSearch);
			joiningResult.removeFirst();
		}

		QString pattern;
		for (int i = 0; i < textToSearch.length(); i++) {
			auto qchar = textToSearch[i];


			if (!markIncluded && qchar.isMark()) continue;

			if (!markIncluded && !pattern.isEmpty()) {
				pattern.append(markClass);
			}

			if (checkBoxShapes && charClusters.contains(qchar)) {

				switch (joiningResult[i].joining)
				{
				case arabic_joining_t::ISOL:
					pattern.append(charClusters[qchar][0]);
					break;
				case arabic_joining_t::INIT:
					pattern.append(charClusters[qchar][1]);
					break;
				case arabic_joining_t::MEDI:
					pattern.append(charClusters[qchar][2]);
					break;
				case arabic_joining_t::FINA:
					pattern.append(charClusters[qchar][3]);
					break;
				default:
					pattern.append(qchar);
				}
			}
			else {
				pattern.append(qchar);
			}

		}

		pattern = QString("(?<capt%1>%2)").arg(type).arg(pattern);

		if (type == 0) {
			if (!checkBoxMarks) {
				pattern = markClass + pattern + markClass;
			}
			if (!checkBoxSubWords) {
				pattern = "^" + pattern + "$";
			}
			else {
				if (firstBase.joiningType() == QChar::Joining_None) {
					pattern = "(^|\\P{Mn}" + markClass + ")" + pattern;
				}
				else {
					pattern = "(^|" + rExpClasses["NoneRight"] + ")" + pattern;
				}
				if (lastBase.joiningType() == QChar::Joining_Dual)
				{
					pattern = pattern + "(" + markClass + rExpClasses["None"] + "|$)";
				}
				else {
					pattern = pattern + "(" + markClass + "\\P{Mn}|$)";
				}


			}

		}
		else if (type == 1) {
			if (!checkBoxMarks) {
				pattern = markClass + pattern;
			}
			if (!checkBoxSubWords) {
				pattern = "^" + pattern + markClass + "\\P{Mn}";
			}
			else {
				pattern = "(^|" + rExpClasses["NoneRight"] + ")" + pattern + markClass + rExpClasses["DualRight"];
			}
		}
		else if (type == 2) {
			if (!checkBoxSubWords) {
				pattern = "\\P{Mn}" + markClass + pattern + markClass + "\\P{Mn}";
			}
			else {
				pattern = rExpClasses["Dual"] + markClass + pattern + markClass + rExpClasses["DualRight"];
			}
		}
		else if (type == 3) {
			if (!checkBoxMarks) {
				pattern = pattern + markClass;
			}
			if (!checkBoxSubWords) {
				pattern = "\\P{Mn}" + markClass + pattern + "$";
			}
			else {
				pattern = rExpClasses["Dual"] + markClass + pattern;
				if (lastBase.joiningType() == QChar::Joining_Dual)
				{
					pattern = pattern + "(" + markClass + rExpClasses["None"] + "|$)";
				}
				else {
					pattern = pattern + "(" + markClass + "\\P{Mn}|$)";
				}
			}
		}
		return pattern;
	};

	QString pattern;


	if (checkBoxIsol) {
		pattern = getPattern(0);
	}
	if (checkBoxInit) {
		auto init = getPattern(1);
		if (!init.isEmpty()) {
			pattern = pattern.isEmpty() ? init : (pattern + "|" + init);
		}
	}
	if (checkBoxMedi) {
		auto medi = getPattern(2);
		if (!medi.isEmpty()) {
			pattern = pattern.isEmpty() ? medi : (pattern + "|" + medi);
		}
	}
	if (checkBoxFina) {
		auto fina = getPattern(3);
		if (!fina.isEmpty()) {
			pattern = pattern.isEmpty() ? fina : (pattern + "|" + fina);
		}
	}

	/*
	if (pattern.isEmpty()) {
		QMessageBox::information(this, tr("Info"),
			QString("Nothing to search"));
		return;
	}*/

	if (!pattern.isEmpty()) {
		QRegularExpression re(pattern, QRegularExpression::UseUnicodePropertiesOption);
		for (int pageIndex = 0; pageIndex < PageAnalysisResult::QuranTextByWord.size(); pageIndex++) {
			auto page = PageAnalysisResult::QuranTextByWord[pageIndex];
			for (int lineIndex = 0; lineIndex < page.size(); lineIndex++) {
				auto line = page[lineIndex];
				for (int wordIndex = 0; wordIndex < line.size(); wordIndex++) {
					auto word = line[wordIndex];
					auto matchIterator = re.globalMatch(word);
					while (matchIterator.hasNext()) {
						matches.push_back(WordMatch{ pageIndex ,lineIndex,wordIndex,0,matchIterator.next() });
					}
				}
			}
		}
	}

	return matches;
}