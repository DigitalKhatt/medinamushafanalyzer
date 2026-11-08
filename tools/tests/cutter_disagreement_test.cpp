// Protect letter ownership in the first ranked disagreement batch and its controls.
#include "../../segmentsubword.cpp"
#include <QApplication>

static void require(bool condition, const QString& message) {
    if (!condition) qFatal("%s", qPrintable(message));
}

static QPainterPath readPath(const QJsonArray& commands) {
    QPainterPath path;
    for (const auto& value : commands) {
        const auto c = value.toArray();
        if (c[0] == "M") path.moveTo(c[1].toDouble(), c[2].toDouble());
        else if (c[0] == "L") path.lineTo(c[1].toDouble(), c[2].toDouble());
        else path.cubicTo(c[1].toDouble(), c[2].toDouble(), c[3].toDouble(),
                          c[4].toDouble(), c[5].toDouble(), c[6].toDouble());
    }
    return path;
}

static bool sameElements(const QPainterPath& a, const QPainterPath& b) {
    if (a.elementCount() != b.elementCount()) return false;
    for (int i = 0; i < a.elementCount(); ++i) {
        const auto p = a.elementAt(i), q = b.elementAt(i);
        if (p.type != q.type || QLineF(QPointF(p), QPointF(q)).length() > 1e-8) return false;
    }
    return true;
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    Q_INIT_RESOURCE(application);
    const auto config = PageAnalysisResult::letterForms();
    require(!config.value("kashidaDefaults").toObject().value("trackingEnabled").toBool(), "Tracker is enabled");
    for (const auto& bad : QJsonArray{true, "", QJsonArray{"ل"}}) {
        auto invalid = config;
        invalid.insert("final", QJsonArray{QJsonObject{{"id", "invalid-ligature"}, {"current", "ا"},
            {"method", "walk"}, {"top", QJsonArray{"down"}}, {"bottomProbe", "normal"},
            {"sharedBoundary", true}, {"sharedBoundaryWith", bad}}});
        require(!PageAnalysisResult::prepareLetterForms(invalid).isEmpty(), "Invalid ligature partner accepted");
    }
    QFile fixture("tools/tests/fixtures/cutter-disagreement-controls.json");
    require(fixture.open(QIODevice::ReadOnly), "Missing disagreement controls");
    const auto controls = QJsonDocument::fromJson(fixture.readAll()).object();
    const auto preserved = controls.value("preserved").toObject();
    QFile file("input/font.dat");
    require(file.open(QIODevice::ReadOnly), "Missing native font");
    AFont font;
    QDataStream fontStream(&file);
    fontStream >> font;
    PageAnalysisResult::initQuranText();
    struct OwnershipWitness {
        QJsonArray address;
        int owner;
        QPointF point;
    };
    const OwnershipWitness restored[] = {
        {{362, 11, 3, 1}, 3, {250.1164, 353.2583}}, // Detached alif diagonal.
        {{9, 11, 1, 3}, 3, {256.6303, 356.3799}}, // Alif shaft after heh.
        {{353, 15, 3, 1}, 1, {252.2808, 465.5955}}, // The lip of initial jim.
        {{101, 4, 8, 1}, 3, {88.7886, 176.1336}}, // Folded yeh bowl.
        {{595, 15, 10, 2}, 1, {78.5451, 460.9974}}, // Descending lam shaft.
        {{121, 14, 8, 1}, 2, {60.9473, 437.0233}}, // Raised reh entry after ain.
        {{91, 3, 4, 1}, 3, {195.7362, 143.0616}}, // Raised reh entry after a tooth.
        {{327, 12, 5, 1}, 1, {184.2087, 386.8838}}, // Lower return of kaf.
        {{316, 5, 11, 1}, 1, {108.3132, 194.6951}}, // Seen tooth above jim.
        {{328, 3, 7, 2}, 1, {140.9783, 137.3301}}, // Stacked noon above jim.
        {{325, 7, 2, 1}, 1, {271.4998, 251.0888}}, // Upright initial tooth.
        {{102, 2, 7, 1}, 3, {188.85, 114.9}}, // The final reh's upper hook.
        {{407, 11, 7, 1}, 3, {100.95, 357.3}}, // Reh hook after the tah loop.
        {{479, 13, 6, 1}, 2, {181.8, 412.8}}, // Broad zay hook after jim.
    };
    auto locations = controls.value("locations").toArray();
    for (const auto& witness : restored)
        locations.append(witness.address);
    locations.append(QJsonArray{92, 12, 2, 3});
    locations.append(QJsonArray{5, 1, 5, 2});
    locations.append(QJsonArray{19, 12, 6, 3});
    locations.append(QJsonArray{589, 14, 7, 3});
    int loaded = 0;
    PageAnalysisResult analysis;
    for (const auto& value : locations) {
        const auto a = value.toArray();
        const int page = a[0].toInt(), line = a[1].toInt()-1, wordIndex = a[2].toInt()-1, subIndex = a[3].toInt()-1;
        const auto id = QString("page%1:l%2-w%3-s%4").arg(page).arg(line+1).arg(wordIndex+1).arg(subIndex+1);
        qInfo().noquote() << "Checking" << id;
        if (loaded != page) {
            QFile source(QString("pages/page%1.dat").arg(page));
            require(source.open(QIODevice::ReadOnly), id + " source missing");
            analysis = {};
            QDataStream stream(&source);
            stream >> analysis.page;
            analysis.resolveWordBoundaries(page, &font);
            loaded = page;
        }
        auto& word = analysis.page.lines[line].words[wordIndex];
        auto& sub = word.wordResultInfo.subWords[subIndex];
        require(analysis.segmentSubword(page-1, line, wordIndex, subIndex, &font, true), id + " became incomplete");
        const auto text = PageAnalysisResult::bodyLetters(sub.text);
        require(sub.baseGlyphs.size() == text.size() && coversOriginalInk(word, sub), id + " lost or duplicated ink");
        const auto shape = word.bodyShape(sub);
        const auto world = [&](const QPainterPath& p) { return shape.transform.map(p).translated(shape.pos); };
        const auto owns = [&](int owner, QPointF point) {
            int matches = 0;
            for (size_t i = 0; i < sub.baseGlyphs.size(); ++i)
                if (world(sub.baseGlyphs[i].path).contains(point)) {
                    require(int(i)+1 == owner, id + " assigned a protected stroke to the wrong letter");
                    ++matches;
                }
            for (const auto& span : sub.kashidaPaths)
                require(!world(span).contains(point), id + " assigned a protected stroke to a kashida");
            require(matches == 1, id + " protected stroke has no unique owner");
        };
        const auto joinsWith = [&](int join, QPointF point) {
            require(join < int(sub.kashidaPaths.size()) && world(sub.kashidaPaths[join]).contains(point),
                    id + " connecting stroke lost its kashida");
        };
        for (const auto& witness : restored)
            if (a == witness.address) owns(witness.owner, witness.point);
        if (a == QJsonArray{102, 2, 7, 1}) owns(2, {191.4, 112.5});
        if (a == QJsonArray{5, 1, 5, 2}) {
            owns(3, {137.5, 90.0}); // The outgoing bend belongs to the medial lam.
            joinsWith(2, {135.5, 90.55});
        }
        if (a == QJsonArray{19, 12, 6, 3}) {
            owns(6, {118.403, 384.066}); // The raised right rim belongs to the noon.
            joinsWith(4, {119.715, 383.316});
        }
        if (a == QJsonArray{589, 14, 7, 3}) {
            owns(2, {124.215, 436.563}); // Keep the outer wall with the bowl.
            joinsWith(0, {124.965, 436.750});
        }
        if (a == QJsonArray{92, 12, 2, 3}) {
            const auto& seam = *sub.joins.rbegin();
            require(std::abs(seam.topBaseLine.time - 8) < 1e-6 &&
                    std::abs(seam.bottomBaseLine.time - 12) < 1e-6 &&
                    sub.kashidaPaths.back().isEmpty(), id + " special reh join changed");
        }
        if (a == QJsonArray{316, 5, 11, 1} || a == QJsonArray{328, 3, 7, 2})
            require(sub.kashidaPaths.front().isEmpty(), id + " stacked entry gained a connector");
        if (a == QJsonArray{362, 11, 3, 1})
            require(sub.letterDiagnostics.last().toObject().value("boundaryKind") == "separate-contours",
                    id + " detached alif was given a chord through a pen lift");
        if (a == QJsonArray{9,15,1,2}) {
            owns(5, {263.25,464.6}); // Raised entrance of the final noon.
            joinsWith(3, {266.0,465.7});
        }
        if (a == QJsonArray{27,3,2,3}) {
            owns(5, {218.65,142.55});
            joinsWith(3, {222.0,143.7});
        }
        // The crest belongs to the terminal bowl/hook; its approach is still a connector.
        struct TerminalEntry {
            QJsonArray address;
            QPointF body;
            QPointF connector;
        };
        const TerminalEntry entries[] = {
            {{482,9,9,3}, {99.6,304.35}, {101.4,304.8}},
            {{584,9,8,3}, {146.05,304.2}, {147.7,304.4}},
            {{267,13,1,3}, {258.85,412.2}, {261.4,412.4}},
            {{589,7,1,3}, {265.0,250.2}, {266.8,250.3}},
            {{203,2,2,2}, {214.25,117.65}, {216.0,117.6}},
            {{464,12,10,2}, {100.05,384.6}, {101.4,384.7}},
            {{406,2,8,1}, {133.5,116.45}, {134.9,115.2}},
            {{131,8,6,1}, {163.6,280.5}, {165.9,277.2}},
        };
        for (const auto& entry : entries) if (a == entry.address) {
            owns(3, entry.body);
            joinsWith(1, entry.connector);
        }
        if (a == QJsonArray{223,1,6,1}) {
            owns(2, {157.1,89.2}); // Ain head must not be counted as a seen tooth.
            owns(3, {148.1,90.75});
        }
        if (a == QJsonArray{454,6,2,1}) owns(3, {285.05,227.65});
        if (a == QJsonArray{176,8,8,2}) owns(3, {118.1,282.0});
        if (a == QJsonArray{372,4,11,1}) owns(3, {96.4,174.6});
        if (a == QJsonArray{207,14,2,2}) joinsWith(0, {225.05,440.85});
        if (a == QJsonArray{578,9,8,1}) {
            owns(3, {152.2,309.6}); // The bottom of the folded yeh.
            joinsWith(1, {156.7,304.45});
            require(sub.kashidaPaths[0].isEmpty(), id + " stacked entry gained a connector");
        }
        if (a == QJsonArray{506,10,4,1}) {
            owns(3, {259.9,335.0});
            joinsWith(1, {264.1,330.2});
            require(sub.kashidaPaths[0].isEmpty(), id + " stacked entry gained a connector");
        }
        if (a == QJsonArray{11,5,6,1}) {
            owns(1, {153.5,197.4}); // Meem rim.
            owns(2, {141.0,195.5}); // Upper lip of kha.
            owns(2, {142.0,197.9});
            joinsWith(0, {148.6,198.9}); // Curved stroke between meem and kha.
            owns(3, {141.0,199.5});
        }
        if (a == QJsonArray{25,6,8,1}) {
            owns(1, {123.0,212.1}); // Kaf's diagonal blade.
            owns(2, {109.5,221.8}); // Upper lip of hah.
        }
        if (a == QJsonArray{74,15,7,1}) {
            owns(2, {155.45,466.6}); // The rise into the final zay.
            joinsWith(0, {159.0,468.4});
        }
        if (a == QJsonArray{367,3,3,1}) owns(2, {245.0,139.0}); // Medial lam's shaft.
        if (a == QJsonArray{298,4,5,1}) { owns(2, {227.3,170.0}); owns(2, {224.5,172.5}); } // Meem loop and tail.
        if (a == QJsonArray{205,11,9,1}) owns(2, {60.5,354.8}); // Upper hah lip.
        if (a == QJsonArray{523,13,9,2}) { owns(3, {67.5,406.1}); owns(3, {63.6,412.7}); } // Noon head and bowl.
        if (a == QJsonArray{483,13,9,1}) {
            owns(1, {115.5,407.7}); // The user keeps the long diagonal with initial yeh.
            owns(2, {113.0,410.85});
            owns(3, {108.0,417.0});
        }
        if (a == QJsonArray{572,10,4,3} || a == QJsonArray{524,11,5,2})
            require(sub.kashidaPaths.size() == 1 && !sub.kashidaPaths[0].isEmpty(),
                    id + " ordinary noon–alef lost its genuine connector");
        if (a == preserved.value("address").toArray()) {
            require(text == QStringLiteral("بيني"), "The four-letter control changed transcript");
            const auto bodies = preserved.value("bodies").toArray(), spans = preserved.value("kashidas").toArray();
            require(sub.baseGlyphs.size() == bodies.size() && sub.kashidaPaths.size() == spans.size(), id + " mask count changed");
            for (int i = 0; i < bodies.size(); ++i)
                require(sameElements(world(sub.baseGlyphs[i].path), readPath(bodies[i].toArray())), id + " confirmed body changed");
            for (int i = 0; i < spans.size(); ++i)
                require(sameElements(world(sub.kashidaPaths[i]), readPath(spans[i].toArray())), id + " confirmed connector changed");
        }
        for (const auto& value : controls.value("additionalPreserved").toArray()) {
            const auto expected = value.toObject();
            if (a != expected.value("address").toArray()) continue;
            const auto bodies = expected.value("bodies").toArray(), spans = expected.value("kashidas").toArray();
            require(sub.baseGlyphs.size() == bodies.size() && sub.kashidaPaths.size() == spans.size(), id + " preserved mask count changed");
            for (int i = 0; i < bodies.size(); ++i)
                require(sameElements(world(sub.baseGlyphs[i].path), readPath(bodies[i].toArray())), id + " ordinary noon control changed");
            for (int i = 0; i < spans.size(); ++i)
                require(sameElements(world(sub.kashidaPaths[i]), readPath(spans[i].toArray())), id + " ordinary noon connector changed");
        }
        if (text == QStringLiteral("شي"))
            require(sub.baseGlyphs.size() == 2 && sub.matchedTemplates.contains("seen-init"), id + " seen became a tooth sequence");
        const auto bodies = sub.baseGlyphs;
        const auto spans = sub.kashidaPaths;
        require(analysis.segmentSubword(page-1, line, wordIndex, subIndex, &font, true), id + " repeated cut failed");
        require(sub.baseGlyphs.size() == bodies.size() && spans == sub.kashidaPaths, id + " is nondeterministic");
        for (size_t i = 0; i < bodies.size(); ++i)
            require(bodies[i].path == sub.baseGlyphs[i].path, id + " body is nondeterministic");
    }
    qInfo() << "Ranked disagreement batch passed:" << locations.size()
            << "subwords with conserved ink, protected body landmarks and deterministic masks.";
}
