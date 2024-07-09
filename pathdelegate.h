#pragma once


class PathImage
{
public:
	enum class EditMode { Editable, ReadOnly };

	explicit PathImage(QPainterPath path = QPainterPath{}) {
		this->setPath(path);

	}

	void paint(QPainter* painter, const QRect& rect,
		const QPalette& palette, EditMode mode) const {

		painter->save();

		painter->setRenderHint(QPainter::Antialiasing, true);

		painter->setPen(Qt::NoPen);

		painter->setBrush(Qt::black);

		const int yOffset = (rect.height());
		auto rec = path.boundingRect();
		painter->translate(rect.x() + (rect.width() - rec.width()) / 2, rect.y() + (rect.height() - rec.height()) / 2);

		painter->drawPath(path);

		painter->restore();

	}
	QSize sizeHint() const {
		auto rec = path.boundingRect();
		return rec.toRect().size() + QSize(20, 20);
	}
	void setPath(QPainterPath path) {
		this->path = path * transform;
		auto box = this->path.boundingRect();
		this->path = this->path.translated(-box.left(), -box.top());
	}

	QPainterPath getPath() {
		return path;
	}

private:
	QPainterPath path;
	QTransform transform{ 3,0,0,3,0,0 };
};
Q_DECLARE_METATYPE(PathImage)

class PathDelegate : public QStyledItemDelegate
{

public:
	using QStyledItemDelegate::QStyledItemDelegate;

	void paint(QPainter* painter, const QStyleOptionViewItem& option,
		const QModelIndex& index) const override {
		if (index.data().canConvert<PathImage>()) {
			PathImage pathImage = qvariant_cast<PathImage>(index.data());

			pathImage.paint(painter, option.rect, option.palette,
				PathImage::EditMode::ReadOnly);
		}
		else {
			QStyledItemDelegate::paint(painter, option, index);
		}

	}
	QSize sizeHint(const QStyleOptionViewItem& option,
		const QModelIndex& index) const override {
		if (index.data().canConvert<PathImage>()) {
			PathImage pathImage = qvariant_cast<PathImage>(index.data());
			return pathImage.sizeHint();
		}
		return QStyledItemDelegate::sizeHint(option, index);
	}


};