#pragma once
#include "letterforminspection.h"
#include <QWidget>

class QPlainTextEdit;
class QTabWidget;
class QGraphicsView;
class QComboBox;
class QLabel;
class QCheckBox;

// Focused editors share one canonical JSON draft with the existing dialog.
// Invalid text remains editable and prevents preview/save, never silently lost.
class BodyCutPanel : public QWidget {
    Q_OBJECT
public:
    explicit BodyCutPanel(QPlainTextEdit* fullEditor, QWidget* parent = nullptr);
    bool selectForm(const QString& id);
    bool readConfig(QJsonObject& config, QString& error) const;
    void setInspection(const LetterFormInspection& inspection);
    void invalidate();
    void showFullEditor();
    QString reviewDetails() const;
signals:
    void draftChanged();
private:
    void loadFields();
    void editField(int field);
    void drawOverlay(int field);
    QPlainTextEdit* full;
    QPlainTextEdit* fields[2];
    QPlainTextEdit* effective[2];
    QGraphicsView* views[2];
    QLabel* help[2];
    QLabel* state;
    QPlainTextEdit* reasons;
    QTabWidget* tabs;
    QComboBox* candidates;
    QCheckBox* labels;
    QString id, errors[2];
    bool syncing = false;
    LetterFormInspection snapshot;
};
