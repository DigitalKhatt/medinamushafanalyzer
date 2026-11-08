#include "pageanalysisresult.h"
#include <QApplication>
#include <QFile>
#include <QJsonDocument>

static void check(bool passed, const QString& message) {
    if (!passed) qFatal("%s", qPrintable(message));
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    Q_INIT_RESOURCE(application);
    const auto config = PageAnalysisResult::letterForms();
    auto initialOnly = config;
    QJsonArray heads;
    // Keep this control focused on the original upright loop heads; the
    // descending feh variant is covered by additional_shape_forms_test.
    for (const auto& value : config.value("initial").toArray())
        if (value.toObject().value("id") == "feh-init" ||
            value.toObject().value("id") == "qaf-init") heads.append(value);
    check(heads.size() == 2, "Expected initial feh and qaf templates");
    initialOnly.insert("initial", heads);
    initialOnly.insert("medial", QJsonArray{});
    initialOnly.insert("final", QJsonArray{});

    QJsonArray report;
    int page3Cases = 0;
    for (int number : {3, 182, 286, 467}) {
        QFile file(QString("pages/page%1.dat").arg(number));
        if (!file.exists() && number != 3) continue;
        check(file.open(QIODevice::ReadOnly), "Required page cache is unavailable");
        PageAnalysisResult analysis;
        QDataStream stream(&file);
        stream >> analysis.page;
        check(stream.status() == QDataStream::Ok, "Invalid page cache");
        int cases = 0, matches = 0;
        for (int li = 0; li < analysis.page.lines.size(); ++li) {
            auto& line = analysis.page.lines[li];
            for (int wi = 0; wi < line.words.size(); ++wi) {
                auto& word = line.words[wi];
                for (int si = 0; si < word.wordResultInfo.subWords.size(); ++si) {
                    auto& sub = word.wordResultInfo.subWords[si];
                    QString letters;
                    for (const auto ch : sub.text) if (!ch.isMark()) letters += ch;
                    if (letters.size() < 2 || !QStringLiteral("فق").contains(letters.front())) continue;
                    const auto id = QString("page%1:l%2-w%3-s%4").arg(number, 3, 10, QChar('0'))
                        .arg(li + 1).arg(wi + 1).arg(si + 1);
                    const auto expected = letters.front() == QChar(0x0641) ? "feh-init" : "qaf-init";
                    const bool complete = analysis.segmentSubword(number - 1, li, wi, si, nullptr, true);
                    const bool matched = !sub.letterDiagnostics.isEmpty() &&
                        sub.letterDiagnostics.first().toObject().value("template") == expected;
                    report.append(QJsonObject{{"id", id}, {"text", sub.text}, {"template", expected},
                        {"matched", matched}, {"complete", complete}, {"letters", sub.letterDiagnostics}});
                    ++cases;
                    matches += matched;
                    if (number != 3) continue;
                    ++page3Cases;
                    check(matched, id + ": initial head was not found");

                    const auto locate = [&]() {
                        analysis.segmentSubword(number - 1, li, wi, si, nullptr, true, &initialOnly);
                        check(sub.matchedTemplates.contains(expected) && sub.joins.size() == 1,
                              id + ": head depends on other body templates");
                        const auto& cut = *sub.joins.begin();
                        return QLineF(cut.topBaseLine.point, cut.bottomBaseLine.point);
                    };
                    const auto chord = locate();
                    auto& shape = word.paths[sub.paths.front()];
                    const auto originalShape = shape;
                    const auto originalText = sub.text;
                    auto source = shape.transform.map(shape.path);
                    source.translate(shape.pos);
                    // The bent-neck fi form must leave the yeh bowl out of the
                    // initial body, rather than merely clipping a small spur.
                    if (letters == QStringLiteral("في")) {
                        auto head = shape.transform.map(sub.baseGlyphs.front().path);
                        head.translate(shape.pos);
                        check(head.boundingRect().left() > source.boundingRect().center().x(),
                              id + ": the initial region includes the yeh bowl");
                    }

                    sub.text = letters.left(1) + QString(letters.size() - 1, QChar(0x0628));
                    const auto renamed = locate();
                    check(QLineF(chord.p1(), renamed.p1()).length() < 1e-7 &&
                          QLineF(chord.p2(), renamed.p2()).length() < 1e-7,
                          id + ": following character names changed the cut");
                    sub.text = originalText;
                    for (double scale : {0.7, 1.6}) {
                        const QTransform mapping(scale, 0, 0, scale, 37, -21);
                        shape.path = mapping.map(source);
                        shape.transform = {};
                        shape.pos = {};
                        const auto transformed = locate();
                        check(QLineF(mapping.map(chord.p1()), transformed.p1()).length() < 1e-3 &&
                              QLineF(mapping.map(chord.p2()), transformed.p2()).length() < 1e-3,
                              id + ": cut is not scale/translation invariant");
                    }
                    if (page3Cases == 1) {
                        std::vector<QPainterPath> parts;
                        QVector<PathExtrema> extrema;
                        ShapeItem::getExtrema(source, parts, extrema);
                        shape.path = parts.front();
                        shape.transform = {};
                        shape.pos = {};
                        analysis.segmentSubword(number - 1, li, wi, si, nullptr, true, &initialOnly);
                        check(!sub.matchedTemplates.contains(expected), "Head without its counter used a generic cut");
                    }
                    shape = originalShape;
                    sub.text = originalText;
                }
            }
        }
        qInfo().noquote() << QString("Page %1: initial feh/qaf matched %2/%3").arg(number).arg(matches).arg(cases);
    }
    check(page3Cases == 13, "Expected all 13 initial feh/qaf cases on page 3");
    const auto reportPath = qEnvironmentVariable("MEDINA_HEAD_REPORT");
    if (!reportPath.isEmpty()) {
        QFile output(reportPath);
        check(output.open(QIODevice::WriteOnly), "Could not write initial-head report");
        output.write(QJsonDocument(QJsonObject{{"examples", report}}).toJson());
    }
    qInfo("Initial heads passed: own-template/character independence, scale/translation invariance, spur rejection and missing-counter abstention.");
}
