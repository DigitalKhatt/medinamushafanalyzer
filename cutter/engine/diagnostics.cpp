#include "../detail.h"

using namespace MedinaCutter;

namespace MedinaCutter {

thread_local QJsonObject completedSearchProfile;

thread_local CutGeometryMemo* cutGeometryMemo = nullptr;

} // namespace MedinaCutter

QJsonObject letterSearchProfile() { return completedSearchProfile; }

namespace MedinaCutter {

void appendGeometryPoint(QByteArray& key, const QPointF& point) {
    appendGeometryKey(key, point.x());
    appendGeometryKey(key, point.y());
}

void appendGeometryAnchor(QByteArray& key, const PathExtrema& point) {
    appendGeometryKey(key, point.pathIndex);
    appendGeometryKey(key, point.time);
    appendGeometryPoint(key, point.point);
}

thread_local LetterFormInspection* formInspection = nullptr;

thread_local bool inspectingForm = false;

thread_local bool inspectionCoordinates = true;

void inspectReason(const char* stage, const QString& reason) {
    if (inspectingForm) ++formInspection->reasons[QString::fromLatin1(stage) + ": " + reason];
}

void inspectReason(const char* stage, const char* reason) {
    if (inspectingForm) inspectReason(stage, QString::fromUtf8(reason));
}

void inspectLandmark(const char* name, const PathExtrema& point) {
    if (!inspectingForm || !inspectionCoordinates) return;
    const auto label = QString::fromUtf8(name) + QString(" · %1").arg(point.time, 0, 'f', 2);
    for (const auto& item : formInspection->landmarks)
        if (item.label == label && item.point == point.point) return;
    formInspection->landmarks.append({label, point.point});
}

void inspectCandidate(const QString& label, const Stretching& left,
                             const std::optional<Stretching>& right) {
    if (!inspectingForm || !inspectionCoordinates) return;
    QVector<QLineF> lines;
    if (right) for (const auto& line : right->cutSegments()) lines.append(line);
    for (const auto& line : left.cutSegments()) lines.append(line);
    for (const auto& item : formInspection->candidates)
        if (item.cuts == lines) return;
    formInspection->candidates.append({label, lines});
}

void recordBoundaryTemplate(WordResultInfo::SubWordInfo& sub, int slot, bool entry, const QString& id) {
    auto letter = sub.letterDiagnostics.at(slot).toObject();
    letter.insert(entry ? "entryTemplate" : "exitTemplate", id);
    sub.letterDiagnostics[slot] = letter;
}

void recordAssembledForm(WordResultInfo::SubWordInfo& sub, int slot, const QString& id) {
    auto letter = sub.letterDiagnostics.at(slot).toObject();
    if (letter.value("status") != "matched")
        sub.templateTrace.removeAll(QString("%1 %2 · %3").arg(letter.value("position").toString(),
            letter.value("character").toString(), letter.value("status").toString()));
    auto candidates = letter.value("candidates").toArray();
    if (!candidates.contains(id)) candidates.append(id);
    letter.insert("candidates", candidates);
    letter.insert("template", id);
    letter.insert("status", "matched");
    sub.letterDiagnostics[slot] = letter;
}

void collectUsedTemplates(WordResultInfo::SubWordInfo& sub) {
    sub.matchedTemplates.clear();
    const auto append = [&](const QString& id) {
        if (!id.isEmpty() && !sub.matchedTemplates.contains(id)) sub.matchedTemplates.append(id);
    };
    for (const auto& value : sub.letterDiagnostics) {
        const auto letter = value.toObject();
        if (letter.value("status") == "matched") append(letter.value("template").toString());
        for (auto field = letter.constBegin(); field != letter.constEnd(); ++field) {
            if (field.key().endsWith("Template")) append(field.value().toString());
        }
    }
}

} // namespace MedinaCutter
