#ifndef RENDERAREA_H
#define RENDERAREA_H

#include <QPainterPath>
#include <QWidget>

class RenderArea : public QWidget
{
    Q_OBJECT

public:
    explicit RenderArea(const QPainterPath &path, QWidget *parent = nullptr);

    QSize minimumSizeHint() const override;
    QSize sizeHint() const override;

public slots:
    void setFillRule(Qt::FillRule rule);    
    void setBrush(const QBrush& brush);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QPainterPath path;
    QBrush brush;
};

#endif // RENDERAREA_H
