#include "graphicsview.h"
#include "mainwindow.h"
#include "qevent.h"
#include "qmenu.h"

void GraphicsView::mouseMoveEvent(QMouseEvent* event) {
	auto pos = mapToScene(event->pos());
	mainView->setMousePosition(pos);
	QGraphicsView::mouseMoveEvent(event);
}

void GraphicsView::mousePressEvent(QMouseEvent* event) {
	if (event->button() != Qt::LeftButton) {
		event->accept();
		return;
	}
	QGraphicsView::mousePressEvent(event);
}

void GraphicsView::contextMenuEvent(QContextMenuEvent* event) {
	QMenu menu;
	//QAction * scaleAct = new QAction("&Scale", this);	
	//connect(newAct, &QAction::triggered, this, &MainWindow::newFile);

	if (!scene()->selectedItems().empty()) {
		menu.addAction("Add");
		menu.addAction("Compare");
		menu.addAction("Add to Shapes");
		menu.addAction("Compare TF");
		menu.addAction("Display SubWord");
		menu.addAction("Save Picture");
		QAction* a = menu.exec(event->globalPos());
		if (a != NULL) {
			if (a->text() == "Add") {
				bool ok;
				QString text = QInputDialog::getText(this, tr("Set Glyph Name"),
					tr("Glyph Name:"), QLineEdit::Normal,
					"", &ok);
				if (ok && !text.isEmpty()) {
					QPainterPath path;
					for (auto& item : scene()->selectedItems()) {
						QGraphicsPathItem* pathItem = (QGraphicsPathItem*)item;
						auto itemPath = pathItem->path();
						path.addPath(itemPath);
					}
					QTransform transform{ constants::SCALE_GLYPH,0,0,-constants::SCALE_GLYPH,0,0 };
					path = path * transform;
					auto box = path.boundingRect();
					path.translate(-box.left(), -box.top());
					AGlyph aglyph{ text, path };
					mainView->font.glyphs.insert(text, aglyph);
				}

			}
			else if (a->text() == "Compare") {
				bool ok;
				QString text = QInputDialog::getText(this, tr("Input Glyph Name"),
					tr("Glyph Name:"), QLineEdit::Normal,
					"", &ok);
				if (ok) {

					QPainterPath path;
					for (auto& item : scene()->selectedItems()) {
						QGraphicsPathItem* pathItem = (QGraphicsPathItem*)item;
						auto itemPath = pathItem->path();
						path.addPath(itemPath);
					}
					auto res = mainView->font.checkGlyph(path, text);
					if (res != -1) {
						QMessageBox::information(this, tr("Result"),
							QString("This shape is similar to <b>") + text + QString("</b>"));
					}
				}

			}
			else if (a->text() == "Add to Shapes") {
				QTransform transform{ constants::SCALE_GLYPH,0,0,-constants::SCALE_GLYPH,0,0 };
				mainView->shapeScene->clear();
				mainView->shapeScene->setSceneRect(QRectF());
				for (auto& item : scene()->selectedItems()) {
					QGraphicsPathItem* pathItem = (QGraphicsPathItem*)item;
					auto itemPath = pathItem->path() * transform;

					

					mainView->shapeScene->addItem(new ShapeItem(itemPath));
				}


			}
			else if (a->text() == "Compare TF") {
				bool ok;
				QString text = QInputDialog::getText(this, tr("Input Glyph Name"),
					tr("Glyph Name:"), QLineEdit::Normal,
					"", &ok);
				if (ok) {

					QPainterPath path;
					for (auto& item : scene()->selectedItems()) {
						QGraphicsPathItem* pathItem = (QGraphicsPathItem*)item;
						auto itemPath = pathItem->path();
						path.addPath(itemPath);
					}
					if (mainView->font.checkGlyph(path, text, CompareMethod::TF) != -1) {
						QMessageBox::information(this, tr("Result"),
							QString("This shape is similar to <b>") + text + QString("</b>"));
					}
				}
			}
			else if (a->text() == "Display SubWord") {

				for (auto& item : scene()->selectedItems()) {
					if (item->data(0).canConvert<WordMatch>()) {
						WordMatch wordMatch = qvariant_cast<WordMatch>(item->data(0));
						auto& page = mainView->getPageResult(wordMatch.pageIndex + 1);
						auto info = page.detectSubWords(wordMatch.pageIndex, wordMatch.lineIndex, wordMatch.wordIndex, &mainView->font, true);
						mainView->displaySubWords(wordMatch.pageIndex, wordMatch.lineIndex, wordMatch.wordIndex, WordResultFlags::NONE);

						break;
					}


				}
			}
			else if (a->text() == "Save Picture") {

				QPainterPath wordPath;
				for (auto& item : scene()->selectedItems()) {
					QGraphicsPathItem* pathItem = (QGraphicsPathItem*)item;
					wordPath.addPath(pathItem->path());
				}

				if (wordPath.isEmpty()) return;

				QFileDialog dialog(this);
				dialog.setFileMode(QFileDialog::AnyFile);

				QString fileName = QFileDialog::getSaveFileName(this, tr("Save File"),
					"test.jpg",
					tr("Images (*.jpg)"));

				if (fileName.isEmpty()) return;				

				 QTransform transform{ constants::SCALE_GLYPH,0,0,-constants::SCALE_GLYPH ,0,0 };
				//QTransform transform{ constants::SCALE_GLYPH * 1.18,0,0,-constants::SCALE_GLYPH * 1.18 ,0,0 };

				wordPath = wordPath * transform;
				auto box1 = wordPath.boundingRect();
				wordPath.translate(-box1.left() + 50, -box1.top() + 50);

				auto bbox = wordPath.boundingRect();

				QImage image(bbox.size().toSize() + QSize(100, 100), QImage::Format_ARGB32_Premultiplied);
				image.fill(qRgba(255, 255, 255, 255));
				QPainter painter(&image);

				painter.setRenderHint(QPainter::Antialiasing, true);

				painter.setPen(Qt::NoPen);

				painter.setBrush(QBrush(qRgba(174, 234, 174, 255)));
				painter.drawPath(wordPath);
				painter.end();



				image.save(fileName, "jpeg");


			}
		}
	}
}