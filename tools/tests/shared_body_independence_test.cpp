// Shared counter/route evidence belongs to the uncut ink, not a neighboring join.
#include "../../cutter/detail.h"
#include <QApplication>

using namespace MedinaCutter;

static void check(bool ok, const char *message) {
    if (!ok)
        qFatal("%s", message);
}

static QByteArray snapshot(const SharedShapeEvidence &e) {
    QByteArray bytes;
    QDataStream out(&bytes, QIODevice::WriteOnly);
    const auto point = [&](const PathExtrema &p) {
        out << p.pathIndex << p.time << p.point << p.isSharp << p.isX << p.isY;
    };
    const auto points = [&](const auto &values) {
        out << quint64(values.size());
        for (const auto &p : values)
            point(p);
    };
    const auto hole = [&](const CounterHole &h) { out << quint64(h.index) << h.box; };
    const auto corner = [&](const CounterCornerEvidence &c) {
        out << c.counter << c.maxDistance << c.upperLimit << c.upper << c.top << c.paired << quint64(c.corners.size());
        for (const auto &p : c.corners) {
            point(p.anchor);
            out << p.tangent << p.distance;
            points(p.oppositeCorners);
        }
    };
    out << e.source << quint64(e.holes.size());
    for (const auto &h : e.holes)
        hole(h);
    out << bool(e.initialNeck);
    if (e.initialNeck) {
        out << e.initialNeck->counter;
        points(e.initialNeck->valleys);
        points(e.initialNeck->normalAnchors);
    }
    out << bool(e.terminalCounter);
    if (e.terminalCounter)
        hole(*e.terminalCounter);
    out << bool(e.terminalContour);
    if (e.terminalContour)
        hole(*e.terminalContour);
    corner(e.terminalNeck);
    points(e.shoulders);
    out << bool(e.bridge);
    if (e.bridge) {
        out << e.bridge->hole;
        point(e.bridge->tip);
        out << quint64(e.bridge->pairs.size());
        for (const auto &pair : e.bridge->pairs) {
            point(pair.first);
            point(pair.second);
        }
    }
    out << quint64(e.loops.size());
    for (const auto &loop : e.loops) {
        hole(loop.anchor);
        out << loop.hole << loop.bandY << loop.valid;
        corner(loop.entry);
        corner(loop.exit);
        out << quint64(loop.entryRoutes.size());
        for (const auto &entry : loop.entryRoutes) {
            point(entry.crest);
            out << entry.gap << bool(entry.route);
            if (entry.route) {
                point(entry.route->start);
                point(entry.route->origin);
                point(entry.route->anchor);
                point(entry.route->topBefore);
                point(entry.route->topEnd);
                out << entry.route->forwardPrevious << entry.route->topPrevious << entry.route->endpointTolerance;
            }
        }
        out << bool(loop.teeth);
        if (loop.teeth)
            for (const auto &tooth : *loop.teeth) {
                point(tooth.tip);
                points(tooth.forwardFlank);
                points(tooth.reverseFlank);
                out << tooth.reverseIsRight << tooth.entryStroke << tooth.requiredEntry << tooth.minEntryRise
                    << bool(tooth.baselineSection);
                const auto section = [&](const Stretching &v) {
                    point(v.topBaseLine);
                    point(v.bottomBaseLine);
                    point(v.topLeft);
                    point(v.topRight);
                    point(v.bottomLeft);
                    point(v.bottomRight);
                    out << v.clockWise << v.referenceWidth << v.length;
                };
                if (tooth.baselineSection)
                    section(*tooth.baselineSection);
                out << quint64(tooth.pairs.size());
                for (const auto &pair : tooth.pairs) {
                    point(pair.bend);
                    section(pair.localSection);
                    for (const auto *scale : {&pair.localScale, &pair.entryScale}) {
                        out << bool(*scale);
                        if (*scale) {
                            out << (*scale)->stroke;
                            points((*scale)->flankSamples);
                        }
                    }
                }
            }
    }
    out << quint64(e.rims.size());
    for (const auto &rim : e.rims) {
        hole(rim.counter);
        points(rim.anchors);
    }
    out << bool(e.route);
    if (e.route) {
        point(e.route->start);
        point(e.route->origin);
        point(e.route->anchor);
        point(e.route->topBefore);
        point(e.route->topEnd);
        out << e.route->forwardPrevious << e.route->topPrevious << e.route->endpointTolerance;
    }
    out << bool(e.head);
    if (e.head) {
        out << e.head->counter << bool(e.head->edge);
        if (e.head->edge)
            point(*e.head->edge);
        out << quint64(e.head->necks.size());
        for (const auto &neck : e.head->necks) {
            point(neck.crest);
            out << neck.distance << bool(neck.route);
            if (neck.route) {
                point(neck.route->start);
                point(neck.route->origin);
                point(neck.route->anchor);
                point(neck.route->topBefore);
                point(neck.route->topEnd);
                out << neck.route->forwardPrevious << neck.route->topPrevious << neck.route->endpointTolerance;
            }
        }
    }
    out << quint64(e.crestedHeads.size());
    for (const auto &head : e.crestedHeads) {
        out << head.counter;
        point(head.crest);
        point(head.rightEdge);
        point(head.leftEdge);
    }
    out << quint64(e.openHeads.size());
    for (const auto &h : e.openHeads) {
        point(h.crest);
        point(h.rightEdge);
        point(h.leftEdge);
        out << h.height;
    }
    out << bool(e.horizontalPair);
    if (e.horizontalPair)
        out << e.horizontalPair->right << e.horizontalPair->left << e.horizontalPair->height;
    out << quint64(e.stackedPairs.size());
    for (const auto &pair : e.stackedPairs) {
        out << pair.upper << pair.lower;
        for (const auto *feet : {&pair.right, &pair.left}) {
            out << quint64(feet->size());
            for (const auto &foot : *feet) {
                point(foot.anchor);
                out << foot.tangent << foot.distance << foot.hasFoot;
            }
        }
    }
    out << bool(e.tail);
    if (e.tail) {
        point(e.tail->tip);
        out << e.tail->curveCount << quint64(e.tail->branches.size());
        for (const auto &b : e.tail->branches) {
            point(b.corner);
            out << b.forward << b.width;
        }
    }
    out << bool(e.returnSeam);
    if (e.returnSeam) {
        point(e.returnSeam->tip);
        point(e.returnSeam->first);
        point(e.returnSeam->second);
        out << e.returnSeam->shoulderX;
    }
    out << quint64(e.pairedRoutes.size());
    for (const auto &body : e.pairedRoutes) {
        point(body.anchor);
        out << quint64(body.retained.size());
        for (auto t : body.retained)
            out << t;
        for (const auto *route : {&body.right, &body.left}) {
            out << bool(*route);
            if (!*route)
                continue;
            point((*route)->start);
            point((*route)->origin);
            point((*route)->anchor);
            point((*route)->topBefore);
            point((*route)->topEnd);
            out << (*route)->forwardPrevious << (*route)->topPrevious << (*route)->endpointTolerance;
        }
    }
    out << quint64(e.bowlNecks.size());
    for (const auto &b : e.bowlNecks) {
        points(b.edges);
        point(b.cursor);
        point(b.section.topBaseLine);
        point(b.section.bottomBaseLine);
        point(b.section.topLeft);
        point(b.section.topRight);
        point(b.section.bottomLeft);
        point(b.section.bottomRight);
        out << b.section.clockWise << b.section.length << bool(b.probe);
        if (b.probe) {
            point(b.probe->extremum);
            out << bool(b.probe->smooth);
            if (b.probe->smooth)
                point(*b.probe->smooth);
        }
    }
    out << quint64(e.clusters.size());
    for (const auto &c : e.clusters) {
        points(c.turns);
        out << c.advancePrevious << c.ordinary << c.wide << c.minimumX;
    }
    out << quint64(e.enclosedLips.size());
    for (const auto &b : e.enclosedLips) {
        out << b.counter;
        point(b.nose);
        for (const auto &side : b.corners)
            points(side);
    }
    out << quint64(e.mouths.size());
    for (const auto &b : e.mouths) {
        point(b.lips.notch);
        point(b.lips.upper);
        point(b.lips.lower);
        point(b.nose);
        points(b.entryCorners);
    }
    out << quint64(e.blades.size());
    for (const auto &b : e.blades) {
        point(b.elbow);
        point(b.tip);
        point(b.turn);
        point(b.fork);
        out << b.forward << b.width;
    }
    out << quint64(e.parallelReturns.size());
    for (const auto &b : e.parallelReturns) {
        point(b.elbow);
        point(b.edge);
        point(b.tip);
        out << b.first.first << b.first.second << b.second.first << b.second.second << b.stroke << b.slope;
        points(b.corners);
    }
    out << quint64(e.terminalStems.size());
    for (const auto &b : e.terminalStems) {
        point(b.crest);
        point(b.limit);
        point(b.bowlEdge);
        out << b.bowlBottom << b.sample << b.stroke << b.traversal;
    }
    out << quint64(e.bowlForks.size());
    for (const auto &b : e.bowlForks) {
        point(b.tail);
        point(b.upper);
        point(b.lower);
        out << b.forward << b.width;
        points(b.caps);
    }
    out << bool(e.attachment);
    if (e.attachment) {
        out << e.attachment->fork << bool(e.attachment->outer);
        if (e.attachment->outer)
            point(*e.attachment->outer);
        out << quint64(e.attachment->loops.size());
        for (const auto &loop : e.attachment->loops) {
            out << loop.index << loop.box;
            points(loop.inner);
            points(loop.forks);
            points(loop.shoulders);
        }
    }
    out << bool(e.layeredHead);
    if (e.layeredHead) {
        out << quint64(e.layeredHead->envelopes.size());
        for (const auto &box : e.layeredHead->envelopes)
            out << box;
        out << quint64(e.layeredHead->necks.size());
        for (const auto &n : e.layeredHead->necks) {
            point(n.cursor);
            point(n.section.topBaseLine);
            point(n.section.bottomBaseLine);
        }
    }
    out << bool(e.uprightStem);
    if (e.uprightStem) {
        point(e.uprightStem->tip);
        out << e.uprightStem->y << e.uprightStem->stroke;
        points(e.uprightStem->turns);
    }
    out << bool(e.descendingShaft);
    if (e.descendingShaft) {
        const auto &b = *e.descendingShaft;
        point(b.tip);
        out << b.section << b.y << b.width << quint64(b.flanks.size());
        for (const auto &f : b.flanks) {
            point(f.start);
            out << f.forward;
        }
        points(b.feet);
    }
    out << quint64(e.taperedArms.size());
    for (const auto &a : e.taperedArms) {
        point(a.first);
        point(a.second);
        for (const auto &cross : a.crossSections)
            out << cross;
        out << a.bounds << a.width << a.junction << a.fit;
    }
    out << quint64(e.componentArms.size());
    for (const auto &a : e.componentArms) {
        out << a.index << a.bounds;
        point(a.top);
        point(a.bottom);
    }
    out << quint64(e.components.size());
    for (const auto &part : e.components) {
        out << part.ink << quint64(part.indices.size());
        for (int i : part.indices)
            out << i;
    }
    out << bool(e.bowlTurn);
    if (e.bowlTurn) {
        point(e.bowlTurn->tip);
        point(e.bowlTurn->turn);
    }
    out << quint64(e.outlines.size());
    for (const auto &shape : e.outlines) {
        out << shape.eye << quint64(shape.counters.size());
        for (const auto &h : shape.counters)
            hole(h);
    }
    return bytes;
}

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    Q_INIT_RESOURCE(application);
    auto config = PageAnalysisResult::letterForms();
    check(PageAnalysisResult::prepareLetterForms(config).isEmpty(), "Invalid declared forms");
    QVector<QJsonObject> forms;
    QSet<QString> shapes;
    for (const auto *phase : {"initial", "medial", "final"})
        for (const auto &value : config.value(phase).toArray()) {
            const auto form = resolvedBodyForm(config, value.toObject());
            if (form.contains("assembly")) {
                check(!form.contains("body") && !form.contains("method") &&
                          form.value("assembly").toObject().value("operation") == "exposed-head",
                      "A composition recipe still masquerades as a shape matcher");
                auto invalid = config, bad = form;
                auto recipe = bad.value("assembly").toObject();
                recipe.insert("bodyForms", QJsonArray{"missing-body"});
                bad.insert("assembly", recipe);
                invalid.insert(phase, QJsonArray{bad});
                check(!PageAnalysisResult::prepareLetterForms(invalid).isEmpty(),
                      "Unknown composition body reference accepted");
                continue;
            }
            check(!form.contains("method") && !form.value("cuts").toObject().isEmpty() &&
                      (sharedShapeDescription(form.value("body").toObject()) ||
                       lamDescription(form.value("body").toObject()) ||
                       toothDescription(form.value("body").toObject())),
                  "A geometric form was not migrated to body/cuts");
            if (!sharedShapeDescription(form.value("body").toObject()))
                continue;
            check(sharedShapeDescription(form.value("body").toObject()) && !form.contains("method") &&
                      !form.contains("loopBoundaries") && !form.contains("counter") && !form.contains("bodyOutline"),
                  "A declared form still dispatches through legacy geometry");
            forms.append(form);
            shapes.insert(form.value("body").toObject().value("shape").toString());
            auto invalid = config, bad = form, body = form.value("body").toObject();
            body.insert("normalCut", true);
            bad.insert("body", body);
            invalid.insert(phase, QJsonArray{bad});
            check(!PageAnalysisResult::prepareLetterForms(invalid).isEmpty(),
                  "Cut policy accepted in body recognition");
            invalid = config;
            bad = form;
            auto policy = form.value("cuts").toObject();
            policy.insert("boundaries", QJsonObject{{"right", "unknown"}, {"left", "free"}});
            bad.insert("cuts", policy);
            invalid.insert(phase, QJsonArray{bad});
            check(!PageAnalysisResult::prepareLetterForms(invalid).isEmpty(), "Unknown boundary policy accepted");
        }
    const auto& schemas = sharedShapeSchemas();
    for (auto schema = schemas.cbegin(); schema != schemas.cend(); ++schema)
        if (!shapes.contains(schema.key()))
            qFatal("No configured template exercises shared shape %s", qPrintable(schema.key()));
    struct Fixture {
        int page, line, word, sub;
    };
    const Fixture fixtures[] = {{3, 2, 9, 1}, {6, 15, 4, 1}, {600, 2, 5, 1}, {230, 11, 3, 1}, {6, 9, 8, 2}};
    int checks = 0;
    for (const auto &f : fixtures) {
        QFile file(QString("pages/page%1.dat").arg(f.page));
        check(file.open(QIODevice::ReadOnly), "Missing source fixture");
        PageAnalysisResult analysis;
        QDataStream in(&file);
        in >> analysis.page;
        const auto &word = analysis.page.lines[f.line - 1].words[f.word - 1];
        const auto shape = word.bodyShape(word.wordResultInfo.subWords[f.sub - 1]);
        WordResultInfo::SubWordInfo empty;
        SearchContext context{.transformedPath = shape.transform.map(shape.path).translated(shape.pos),
                              .subword = empty};
        check(readTerminalOutline(context), "Missing source outline");
        OutlineSearchCache cache;
        context.outlineCache = &cache;
        for (const auto &form : forms) {
            const auto body = form.value("body").toObject();
            const auto expected = snapshot(recognizeSharedShape(inkOutline(context), body));
            const auto saved = recognizedSharedShape(context, body);
            context.nextIter = context.xSorted.end();
            context.pairedStrokeWidth = 10000;
            context.baseText = QStringLiteral("unrelated");
            context.joinDefaults = {{"enabled", false}};
            context.form = {{"bottomProbe", "invalid"}, {"minRise", 1e9}};
            context.res.topBaseLine.point = {-1e6, 1e6};
            empty.joins.insert(context.res);
            if (snapshot(recognizeSharedShape(inkOutline(context), body)) != expected)
                qFatal("%s on page%d: neighbor/cursor/width state changed intrinsic evidence",
                       qPrintable(form.value("id").toString()), f.page);
            auto edited = form;
            auto policy = form.value("cuts").toObject();
            policy.insert("counter", QJsonObject{{"offset", 1000}});
            edited.insert("cuts", policy);
            check(recognizedSharedShape(context, edited.value("body").toObject()) == saved &&
                      snapshot(*saved) == expected,
                  "A policy edit changed cached recognition");
            ++checks;
        }
    }
    qInfo("Shared evidence passed: %d fresh recognition/neighbor/cache comparisons, %d declarations and %d shapes.",
          checks, forms.size(), shapes.size());
}
