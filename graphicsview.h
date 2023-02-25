#include <QGraphicsView>

class MainWindow;

class GraphicsView : public QGraphicsView
{   
    Q_OBJECT
public:
    GraphicsView(MainWindow* v) : QGraphicsView((QWidget*)v), mainView(v) { 
        setMouseTracking(true);
    }

    friend MainWindow;

protected:
    void mouseMoveEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    
private:
    MainWindow* mainView;
};