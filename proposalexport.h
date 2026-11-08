#pragma once
#include <QString>
int exportReviewProposals(const QString& output, const QString& svgDirectory, const QString& pageSelection,
                         bool includeTemplateScores = true, const QString& markOutput = {}, bool fresh = false);
int exportReviewBaselines(const QString& output, const QString& pageSelection);
int exportReviewTranscript(const QString& output, const QString& pageSelection);
int exportLetterTraining(const QString& output, const QString& pageSelection);
int segmentCatalogPage(const QString& request, const QString& output);
