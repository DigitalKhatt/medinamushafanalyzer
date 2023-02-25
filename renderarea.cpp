
#include "renderarea.h"

#include <QPainter>


RenderArea::RenderArea(const QPainterPath &path, QWidget *parent)
    : QWidget(parent)
{
    setBackgroundRole(QPalette::Base);

    QTransform transform{ 3,0,0,3,0,0 };

    this->path = path  * transform;
    auto box = this->path.boundingRect();
    this->path = this->path.translated(-box.left(), -box.top());
    brush = Qt::black;
    /*
    if (path.elementCount() > 0) {
        auto el = path.elementAt(0);
        if (el.isMoveTo()) {
            QTransform transform{ 2,0,0,-2,0,0 };
            this->path = path.translated(-el.x, -el.y);
            this->path = this->path * transform;
            auto box = this->path.boundingRect();
            this->path = this->path.translated(-box.left(), -box.top());
        }

    }*/
    //setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
}

QSize RenderArea::minimumSizeHint() const
{
    //return QSize(50, 50);
    auto box = this->path.boundingRect().size();
    return box.toSize();
}

QSize RenderArea::sizeHint() const
{    
    //return QSize(100, 100);
    auto box =  this->path.boundingRect().size();
    return box.toSize();
}

void RenderArea::setFillRule(Qt::FillRule rule)
{
    path.setFillRule(rule);
    update();
}
void RenderArea::setBrush(const QBrush& brush) {
    this->brush = brush;
    update();
}

void RenderArea::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    auto widthh = width();
    auto heighth = height();

    //painter.scale(width() / 100.0, height() / 100.0);

    painter.setPen(Qt::NoPen);
    QLinearGradient gradient(0, 0, 0, 100);
    painter.setBrush(brush);
    painter.drawPath(path);
}