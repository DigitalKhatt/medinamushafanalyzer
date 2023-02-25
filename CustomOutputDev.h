#include "CommonPCH.h"
#include "OutputDev.h"
#include "qpainter.h"
#include <QtWidgets/qgraphicsitem.h>
#include <stack>

class PDFDoc;
class CustomOutputDev : public OutputDev
{
public:
    explicit CustomOutputDev();

    ~CustomOutputDev() override;

    void startDoc(PDFDoc* docA);

    //----- get info about output device

   // Does this device use upside-down coordinates?
   // (Upside-down means (0,0) is the top left corner of the page.)
    bool upsideDown() override { return true; }

    // Does this device use drawChar() or drawString()?
    bool useDrawChar() override { return true; }

    // Does this device implement shaded fills (aka gradients) natively?
    // If this returns false, these shaded fills
    // will be reduced to a series of other drawing operations.
    // type==2 is 'axial shading'
    bool useShadedFills(int type) override { return type == 2; }

    // Does this device use beginType3Char/endType3Char?  Otherwise,
    // text in Type 3 fonts will be drawn with drawChar/drawString.
    bool interpretType3Chars() override { return false; }

    //----- initialization and control

    // Set Current Transformation Matrix to a fixed matrix given in ctm[0],...,ctm[5]
    void setDefaultCTM(const double* ctm) override;

    // Start a page.
    void startPage(int pageNum, GfxState* state, XRef* xref) override;

    // End a page.
    void endPage() override;

    //----- save/restore graphics state
    void saveState(GfxState* state) override;
    void restoreState(GfxState* state) override;

    

    //----- update graphics state
    void updateAll(GfxState* state) override;
    void updateCTM(GfxState* state, double m11, double m12, double m21, double m22, double m31, double m32) override;
    /*    
    void updateLineDash(GfxState* state) override;
    void updateFlatness(GfxState* state) override;
    void updateLineJoin(GfxState* state) override;
    void updateLineCap(GfxState* state) override;
    void updateMiterLimit(GfxState* state) override;
    void updateLineWidth(GfxState* state) override;    
    void updateStrokeColor(GfxState* state) override;
    void updateBlendMode(GfxState* state) override;    
    void updateStrokeOpacity(GfxState* state) override;

    //----- update text state
    void updateFont(GfxState* state) override;*/

    void updateFillColor(GfxState* state) override;
    void updateFillOpacity(GfxState* state) override;

    //----- path painting
    void stroke(GfxState* state) override;
    void fill(GfxState* state) override;
    void eoFill(GfxState* state) override;
    bool axialShadedFill(GfxState* state, GfxAxialShading* shading, double tMin, double tMax) override;

    //----- path clipping
    void clip(GfxState* state) override;
    void eoClip(GfxState* state) override;
    void clipToStrokePath(GfxState* state) override;

    /*
    //----- text drawing
    //   virtual void drawString(GfxState *state, GooString *s);
    void drawChar(GfxState* state, double x, double y, double dx, double dy, double originX, double originY, CharCode code, int nBytes, const Unicode* u, int uLen) override;
    void endTextObject(GfxState* state) override;

    //----- image drawing
    void drawImageMask(GfxState* state, Object* ref, Stream* str, int width, int height, bool invert, bool interpolate, bool inlineImg) override;
    void drawImage(GfxState* state, Object* ref, Stream* str, int width, int height, GfxImageColorMap* colorMap, bool interpolate, const int* maskColors, bool inlineImg) override;

    void drawSoftMaskedImage(GfxState* state, Object* ref, Stream* str, int width, int height, GfxImageColorMap* colorMap, bool interpolate, Stream* maskStr, int maskWidth, int maskHeight, GfxImageColorMap* maskColorMap,
        bool maskInterpolate) override;

    //----- Type 3 font operators
    void type3D0(GfxState* state, double wx, double wy) override;
    void type3D1(GfxState* state, double wx, double wy, double llx, double lly, double urx, double ury) override;

    //----- transparency groups and soft masks
    void beginTransparencyGroup(GfxState* state, const double* bbox, GfxColorSpace* blendingColorSpace, bool isolated, bool knockout, bool forSoftMask) override;
    void endTransparencyGroup(GfxState* state) override;
    void paintTransparencyGroup(GfxState* state, const double* bbox) override;*/

    const std::vector<APathItem> getPaths() const
    {
        return paths;
    }

private:
    PDFDoc* doc = nullptr; // the current document
    XRef* xref = nullptr; // the xref of the current document
    std::vector<APathItem> paths;
    QBrush m_currentBrush;
    std::stack<QBrush> m_currentBrushStack;
};