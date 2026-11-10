#pragma once

#include <QJsonObject>
#include <QPainterPath>

class PageAnalysisResult;
struct AFont;

// Last outer subword search on this thread; populated only with MEDINA_PROFILE.
// Diagnostic counters never affect the segmentation or catalog cache revision.
QJsonObject letterSearchProfile();

// Build native cuts for export directly, without writing an intermediate GUI cache.
QJsonObject generateLetterSegmentation(const PageAnalysisResult& source, int pageNumber, AFont& font,
                                       const QJsonObject* requestedSubwords = nullptr);

// The same page-coordinate vector data is used for the overlay and JSON export.
QJsonObject loadLetterSegmentation(const PageAnalysisResult& source, int pageNumber,
                                   AFont& font, bool* cacheHit = nullptr, bool forceRecompute = false);
bool saveLetterSegmentation(const QString& filename, const QJsonObject& data, QString* error = nullptr);
QPainterPath letterSegmentationPath(const QJsonObject& data);
