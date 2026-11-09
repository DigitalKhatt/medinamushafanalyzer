#include "bodycutpanel.h"
#include <QtWidgets>
#include <cmath>

namespace {
class EvidenceView : public QGraphicsView {
public:
    using QGraphicsView::QGraphicsView;
    void resizeEvent(QResizeEvent* event) override {
        QGraphicsView::resizeEvent(event);
        // A hidden tab has no final viewport dimensions yet.
        QTimer::singleShot(0, this, [this] {
            if (scene() && !scene()->items().isEmpty()) fitInView(sceneRect(), Qt::KeepAspectRatio);
        });
    }
    void showEvent(QShowEvent* event) override {
        QGraphicsView::showEvent(event);
        QTimer::singleShot(0, this, [this] {
            if (scene() && !scene()->items().isEmpty()) fitInView(sceneRect(), Qt::KeepAspectRatio);
        });
    }
    void wheelEvent(QWheelEvent* event) override {
        const double before = transform().m11();
        const double after = std::clamp(before * std::pow(1.2, event->angleDelta().y() / 120.), .02, 500.);
        scale(after / before, after / before);
        event->accept();
    }
};
const char* fieldNames[] = {"body", "cuts"};
QJsonObject findForm(const QJsonObject& config, const QString& id) {
    for (const auto* phase : {"initial", "medial", "final"})
        for (const auto& value : config.value(phase).toArray())
            if (value.toObject().value("id") == id) return value.toObject();
    return {};
}
bool parseObject(const QString& text, QJsonObject& object, QString& error) {
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(text.toUtf8(), &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject()) {
        error = parse.error == QJsonParseError::NoError ? "Expected a JSON object." :
            QString("%1 (offset %2)").arg(parse.errorString()).arg(parse.offset);
        return false;
    }
    object = document.object();
    return true;
}
}

BodyCutPanel::BodyCutPanel(QPlainTextEdit* fullEditor, QWidget* parent) : QWidget(parent), full(fullEditor) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    tabs = new QTabWidget(this);
    tabs->setObjectName("bodyCutTabs");
    layout->addWidget(tabs, 1);
    for (int i = 0; i < 2; ++i) {
        auto* pane = new QWidget;
        auto* box = new QVBoxLayout(pane);
        help[i] = new QLabel;
        help[i]->setWordWrap(true);
        box->addWidget(help[i]);
        auto* controls = new QHBoxLayout;
        if (i == 0) {
            labels = new QCheckBox(tr("Landmark labels"));
            controls->addWidget(labels);
            connect(labels, &QCheckBox::toggled, this, [this] { drawOverlay(0); });
        } else {
            candidates = new QComboBox;
            candidates->setObjectName("cutCandidate");
            candidates->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
            candidates->setMinimumContentsLength(20);
            controls->addWidget(candidates, 1);
            connect(candidates, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] { drawOverlay(1); });
        }
        auto* fit = new QPushButton(tr("Fit"));
        controls->addWidget(fit);
        box->addLayout(controls);
        auto* split = new QSplitter(Qt::Vertical);
        views[i] = new EvidenceView;
        views[i]->setObjectName(i == 0 ? "bodyEvidenceView" : "cutPolicyView");
        views[i]->setScene(new QGraphicsScene(views[i]));
        views[i]->setBackgroundBrush(Qt::white);
        views[i]->setRenderHint(QPainter::Antialiasing);
        views[i]->setDragMode(QGraphicsView::ScrollHandDrag);
        views[i]->setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
        views[i]->setMinimumHeight(130);
        views[i]->setToolTip(tr("Scroll to zoom; drag to pan. Hover over a landmark for its contour point."));
        split->addWidget(views[i]);
        connect(fit, &QPushButton::clicked, this, [this, i] {
            views[i]->fitInView(views[i]->sceneRect(), Qt::KeepAspectRatio);
        });
        fields[i] = new QPlainTextEdit;
        fields[i]->setObjectName(i == 0 ? "bodyDraft" : "cutsDraft");
        fields[i]->setLineWrapMode(QPlainTextEdit::NoWrap);
        fields[i]->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        split->addWidget(fields[i]);
        split->setSizes({180, 230});
        box->addWidget(split, 1);
        auto* inherited = new QCheckBox(tr("Show effective settings (including inheritance, read only)"));
        box->addWidget(inherited);
        effective[i] = new QPlainTextEdit;
        effective[i]->setReadOnly(true);
        effective[i]->setMaximumHeight(130);
        effective[i]->setFont(fields[i]->font());
        effective[i]->hide();
        box->addWidget(effective[i]);
        connect(inherited, &QCheckBox::toggled, effective[i], &QWidget::setVisible);
        connect(fields[i], &QPlainTextEdit::textChanged, this, [this, i] { editField(i); });
        tabs->addTab(pane, i == 0 ? tr("Body") : tr("Cut policy"));
    }
    tabs->addTab(full, tr("All templates"));
    state = new QLabel(tr("Select a lām or medial senna template, then preview."));
    state->setWordWrap(true);
    layout->addWidget(state);
    reasons = new QPlainTextEdit;
    reasons->setObjectName("bodyCutReasons");
    reasons->setReadOnly(true);
    reasons->setMaximumHeight(145);
    reasons->setPlaceholderText(tr("Preview diagnostics: recognition, cut placement and final assembly."));
    layout->addWidget(reasons);
    connect(full, &QPlainTextEdit::textChanged, this, [this] {
        if (syncing) return;
        errors[0].clear(); errors[1].clear();
        loadFields();
        emit draftChanged();
    });
}

bool BodyCutPanel::readConfig(QJsonObject& config, QString& error) const {
    for (int i = 0; i < 2; ++i) if (!errors[i].isEmpty()) {
        error = QString::fromLatin1(fieldNames[i]) + ": " + errors[i];
        return false;
    }
    return parseObject(full->toPlainText(), config, error);
}

bool BodyCutPanel::selectForm(const QString& selected) {
    QJsonObject config; QString error;
    if (!readConfig(config, error)) { state->setText(error); return false; }
    id = selected;
    loadFields();
    invalidate();
    return true;
}

void BodyCutPanel::loadFields() {
    QJsonObject config; QString error;
    const bool valid = parseObject(full->toPlainText(), config, error);
    const auto form = findForm(config, id);
    const bool declared = valid && form.value("body").isObject() && form.value("cuts").isObject();
    const auto resolved = declared ? resolvedLetterForm(config, id) : QJsonObject{};
    syncing = true;
    for (int i = 0; i < 2; ++i) {
        fields[i]->setEnabled(declared);
        fields[i]->setPlainText(declared ? QString::fromUtf8(QJsonDocument(form.value(fieldNames[i]).toObject()).toJson()) : QString{});
        effective[i]->setPlainText(QString::fromUtf8(QJsonDocument(resolved.value(fieldNames[i]).toObject()).toJson()));
        help[i]->setText(!declared ? tr("This template has not migrated to body/cuts. Use All templates.") : i == 0 ?
            tr("%1 — intrinsic shape settings. Blue landmarks show search evidence from uncut ink; hover for contour points. Edit this form's overrides; inherited values are shown below.").arg(id) :
            tr("%1 — boundary placement. Cyan lines show the selected candidate; green is the assembled body. Lām forkOffset is in page units; senna offsets use local stroke widths.").arg(id));
    }
    syncing = false;
}

void BodyCutPanel::editField(int field) {
    if (syncing) return;
    QJsonObject value, config; QString error;
    errors[field].clear();
    if (!parseObject(fields[field]->toPlainText(), value, errors[field]) ||
        !parseObject(full->toPlainText(), config, error)) {
        state->setText(errors[field].isEmpty() ? error : errors[field]);
        emit draftChanged();
        return;
    }
    for (const auto* phase : {"initial", "medial", "final"}) {
        auto forms = config.value(phase).toArray();
        for (int index = 0; index < forms.size(); ++index) {
            auto form = forms[index].toObject();
            if (form.value("id") != id) continue;
            form.insert(fieldNames[field], value);
            forms[index] = form;
            config.insert(phase, forms);
        }
    }
    syncing = true;
    full->setPlainText(QString::fromUtf8(QJsonDocument(config).toJson()));
    const auto resolved = resolvedLetterForm(config, id);
    for (int i = 0; i < 2; ++i)
        effective[i]->setPlainText(QString::fromUtf8(QJsonDocument(resolved.value(fieldNames[i]).toObject()).toJson()));
    syncing = false;
    emit draftChanged();
}

void BodyCutPanel::invalidate() {
    snapshot = {};
    for (auto* view : views) view->scene()->clear();
    candidates->clear();
    reasons->clear();
    state->setText(tr("Preview to refresh body evidence, cuts and rejection reasons."));
}

void BodyCutPanel::setInspection(const LetterFormInspection& inspection) {
    snapshot = inspection;
    candidates->clear();
    for (const auto& item : snapshot.candidates) candidates->addItem(item.label);
    drawOverlay(0); drawOverlay(1);
    state->setText(tr("%1 · %2 landmarks · %3 cut options (includes assembled cuts).")
        .arg(snapshot.formId).arg(snapshot.landmarks.size()).arg(snapshot.candidates.size()));
    reasons->setPlainText(reviewDetails());
}

QString BodyCutPanel::reviewDetails() const {
    if (snapshot.formId.isEmpty()) return {};
    QString text = tr("Final assembly\n") + snapshot.assembly.join('\n');
    text += tr("\n\nSearch diagnostics (counts include retries)\n");
    if (snapshot.reasons.isEmpty()) text += tr("No detailed rejection reason was recorded. See final assembly above.");
    for (auto it = snapshot.reasons.cbegin(); it != snapshot.reasons.cend(); ++it)
        text += QString("%1 × %2\n").arg(it.value()).arg(it.key());
    text += tr("\n\nFull preview trace\n") + snapshot.trace.join('\n');
    return text;
}

void BodyCutPanel::showFullEditor() { tabs->setCurrentIndex(2); }

void BodyCutPanel::drawOverlay(int field) {
    auto* view = views[field]; auto* scene = view->scene();
    scene->clear();
    if (snapshot.ink.isEmpty()) return;
    scene->addPath(snapshot.ink, Qt::NoPen, QColor("#dce1e6"));
    QPen pen(QColor("#087eaa")); pen.setCosmetic(true); pen.setWidthF(2);
    if (field == 0) {
        for (const auto& landmark : snapshot.landmarks) {
            auto* dot = scene->addEllipse(-3, -3, 6, 6, pen, QColor("#087eaa"));
            dot->setFlag(QGraphicsItem::ItemIgnoresTransformations);
            dot->setPos(landmark.point);
            dot->setToolTip(landmark.label);
            if (labels->isChecked()) {
                auto* label = scene->addSimpleText(landmark.label);
                label->setFlag(QGraphicsItem::ItemIgnoresTransformations);
                label->setBrush(QColor("#12384b"));
                label->setPos(landmark.point + QPointF(.15, .15));
            }
        }
    } else {
        scene->addPath(snapshot.selectedBody, Qt::NoPen, QColor("#9ccc99"));
        const int option = candidates->currentIndex();
        if (option >= 0 && option < snapshot.candidates.size())
            for (const auto& cut : snapshot.candidates[option].cuts) scene->addLine(cut, pen);
    }
    view->setSceneRect(snapshot.ink.boundingRect().adjusted(-.75, -.75, .75, .75));
    view->fitInView(view->sceneRect(), Qt::KeepAspectRatio);
}
