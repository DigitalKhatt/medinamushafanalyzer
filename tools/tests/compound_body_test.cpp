#include "afont.h"
#include "pageanalysisresult.h"
#include "GlobalParams.h"
#include <QApplication>

static QPainterPath pagePath(const Apath& shape) {
    return shape.transform.map(shape.path).translated(shape.pos);
}

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    Q_INIT_RESOURCE(application);
    globalParams = std::make_unique<GlobalParams>();
    PageAnalysisResult::initQuranText();
    AFont font;
    QFile fontFile("input/font.dat");
    if (!fontFile.open(QIODevice::ReadOnly)) qFatal("Missing native font");
    QDataStream fontStream(&fontFile);
    fontStream >> font;

    struct Case { int page, line, word, sub, secondary; bool counter; };
    for (const Case sample : {Case{189,15,1,3,5,false}, Case{203,7,8,1,4,false},
                              Case{219,6,5,1,0,false}, Case{437,1,7,2,3,true},
                              Case{259,10,8,2,5,false}, Case{536,14,8,1,4,false},
                              Case{563,13,2,1,6,false}}) {
        PageAnalysisResult analysis;
        if (analysis.loadPage(sample.page, &font, false) < 0) qFatal("Fresh PDF segmentation failed");
        auto& line = analysis.page.lines[sample.line - 1];
        const auto& word = line.words[sample.word - 1];
        const auto& sub = word.wordResultInfo.subWords.at(sample.sub - 1);
        if (sub.secondaryBodyPath != sample.secondary || sub.bodyCounter != sample.counter)
            qFatal("Page %d: incorrect body membership in the live native segmenter", sample.page);
        const auto primary = pagePath(word.paths[sub.paths.front()]);
        const auto secondary = pagePath(word.paths[sample.secondary]);
        const auto joined = pagePath(word.bodyShape(sub));
        auto winding = joined;
        winding.setFillRule(Qt::WindingFill);
        if (joined.elementCount() != primary.elementCount() + secondary.elementCount())
            qFatal("Page %d: source vector commands changed", sample.page);
        const auto bounds = primary.boundingRect().united(secondary.boundingRect());
        int interior = 0;
        for (double y = bounds.top() + .05; y < bounds.bottom(); y += 0.1)
            for (double x = bounds.left() + .05; x < bounds.right(); x += 0.1) {
                const QRectF sampleArea(x - .04, y - .04, .08, .08);
                if ((primary.intersects(sampleArea) && !primary.contains(sampleArea)) ||
                    (secondary.intersects(sampleArea) && !secondary.contains(sampleArea))) continue;
                const QPointF point(x, y);
                const bool a = primary.contains(point), b = secondary.contains(point);
                if (b) ++interior;
                const bool expected = sample.counter ? a && !b : a || b;
                if (joined.contains(point) != expected || winding.contains(point) != expected)
                    qFatal("Page %d: composed vector body differs from its source contours", sample.page);
            }
        if (!interior) qFatal("The secondary source contour is empty");
    }
    qInfo() << "Passed live vector body checks on seven source pages";
}
