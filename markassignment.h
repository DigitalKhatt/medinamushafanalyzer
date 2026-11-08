#pragma once
#include <QJsonObject>
#include <QString>

// Native, classifier-independent mark ownership and exact-ink grouping.
// Input is the native vector export plus QuranTextByWord character metadata.
QJsonObject assignNativeMarks(const QJsonObject &page, const QJsonObject &transcript,
                              const QJsonObject &geometry = {});
int exportNativeMarkAssignments(const QString &inputDirectory, const QString &outputDirectory,
                                const QString &pages);
// Batch JSON operations for native unit/fixture tests; no Python implementation.
int testNativeMarkAssignment(const QString &inputFile, const QString &outputFile);
