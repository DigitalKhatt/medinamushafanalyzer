#pragma once

#include <QJsonObject>
#include <QLineF>
#include <QMap>
#include <QPainterPath>
#include <QStringList>
#include <QVector>

struct PageAnalysisResult;
struct AFont;

// Preview-only evidence, in page coordinates. Never serialized into the catalog.
struct LetterFormInspection {
    struct Landmark { QString label; QPointF point; };
    struct Candidate { QString label; QVector<QLineF> cuts; };
    QString formId;
    QPainterPath ink, selectedBody;
    QVector<Landmark> landmarks;
    QVector<Candidate> candidates;
    QMap<QString, int> reasons;
    QStringList assembly, trace;
    QJsonObject body, cuts;
    bool complete = false;
    bool attempted = false;
};

QJsonObject resolvedLetterForm(const QJsonObject& config, const QString& id);
// Recompute only the caller's private sample. Production segmentation does not
// allocate diagnostics or change its candidate ordering when inspection is off.
LetterFormInspection inspectLetterForm(PageAnalysisResult& sample, int page, int line, int word,
    int subword, AFont* font, const QJsonObject& config, const QString& id);
