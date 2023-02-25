#pragma once
#include "CommonPCH.h"



struct WordMatch {
	int pageIndex;
	int lineIndex;
	int wordIndex;
	QRegularExpressionMatch match;
	QPainterPath path;
};
Q_DECLARE_METATYPE(WordMatch)

class QuranSearch {

public:
	QuranSearch();

	std::vector<WordMatch> searchText(QString textToSearch, bool checkBoxMarks, bool checkBoxSubWords,
		bool checkBoxShapes, bool checkBoxIsol, bool checkBoxInit, bool checkBoxMedi, bool checkBoxFina);	

private:

	void initRexpClasses();

	

	QMap<QChar, QVector<QString>> charClusters;
	QMap<QString, QString> rExpClasses;

	QString markClass = "[\\p{Mn}]*";

	QString seqAny = ".*\\P{Mn}.*";

};
