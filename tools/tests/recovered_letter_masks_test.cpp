// Verify which letter owns selected strokes in shapes that previously stopped early.
#include "pageanalysisresult.h"
#include "afont.h"
#include <QApplication>
#include <QFile>
#include <optional>

namespace {
struct Witness {
    int letter;    // Reading order, one-based.
    QPointF point; // Native page coordinates.
};

struct ConnectorWitness {
    size_t afterLetter; // Reading order, one-based.
    QPointF ink;
    double horizontalSpan;
};

struct ConnectorCap {
    size_t afterLetter;
    bool receiving;
    QPointF top, bottom;
};

struct Example {
    int page, line, word, subword;
    std::vector<Witness> witnesses;
    std::optional<ConnectorWitness> connector{};
    std::vector<ConnectorCap> caps{};
};

void require(bool condition, const QString& reason) {
    if (!condition)
        qFatal("%s", qPrintable(reason));
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    Q_INIT_RESOURCE(application);
    AFont font;
    QFile fontFile("input/font.dat");
    require(fontFile.open(QIODevice::ReadOnly), "Missing native font");
    QDataStream fontStream(&fontFile);
    fontStream >> font;
    PageAnalysisResult::initQuranText();

    const Example examples[] = {
        // The tight attachment keeps the hah lip and the qaf bowl on their own sides.
        {23, 6, 7, 1, {{3, {91.59, 223.58}}, {4, {85.97, 228.26}}}},
        // Both lips belong to medial hah, with its neighboring tooth and waw intact.
        {169, 7, 6, 2, {{2, {163.03, 247.39}}, {2, {162.65, 251.14}},
                         {3, {155.71, 249.64}}, {4, {149.53, 255.26}}}},
        // The folded jim–yeh attachment keeps the head and bowl with a single seam.
        {220, 9, 9, 1, {{3, {109.40, 300.26}}, {3, {111.28, 302.51}}, {4, {103.03, 308.13}}}},
        // Initial jim ends at its lip. Final noon keeps the full bowl.
        {422, 5, 4, 2, {{1, {250.96, 194.14}}, {1, {253.77, 195.83}}, {2, {243.09, 202.02}}}},
        // The three teeth share one seen body even when the final crest has a tiny subdivision.
        {510, 9, 3, 2, {{2, {276.46, 306.45}}, {2, {274.77, 305.70}}, {2, {271.59, 305.88}}}},
        // The counter projection still needs to be normal when there is no corner pair.
        {583, 11, 8, 2, {{2, {109.03, 355.01}}, {2, {103.03, 359.69}}}, {},
         {{1, true, {109.844, 358.149}, {109.579, 359.355}}}},
        // The rising entry and folded shoulder each need a normal through their own anchor.
        {17, 9, 9, 1, {}, {}, {{1, true, {76.822, 303.395}, {75.948, 304.306}}}},
        {204, 6, 3, 1, {}, {}, {{2, true, {261.547, 223.941}, {260.987, 224.213}}}},
        // The tooth and its descending edge belong to ṣād/ḍād, including on a folded join.
        {37, 11, 9, 1, {{2, {59.20, 358.10}}, {2, {59.08, 358.14}}}},
        {88, 4, 1, 1, {{1, {326.42, 171.50}}, {1, {326.58, 171.67}}}},
        {173, 12, 8, 2, {{2, {63.73, 384.45}}, {2, {63.35, 384.75}}}},
        // Keep ṣād's exit and put the approach to final ṭāʾ into the connector.
        {39, 15, 4, 2, {{3, {162.0, 464.18}}, {4, {159.90, 465.58}}},
         ConnectorWitness{3, {160.26, 465.30}, .864},
         {{3, false, {160.866, 464.626}, {161.336, 465.269}},
          {3, true, {160.027, 465.003}, {160.447, 465.911}}}},
        {412, 15, 1, 3, {{2, {313.04, 464.45}}}},
        // The cut at the lower shoulder keeps the raised part of final reh.
        {315, 7, 3, 1, {{3, {223.27, 250.22}}, {4, {222.45, 249.80}}, {4, {222.32, 250.34}}}},
        // The descending stem owns its arm up to the shared yeh fold.
        {286, 6, 2, 2, {{3, {277.77, 225.64}}, {4, {278.50, 227.65}}}},
        {154, 15, 5, 1, {{2, {231.27, 465.60}}, {3, {232.26, 467.80}}}},
        // A rounded lām foot can be farther from the curve endpoint than snapping permits.
        {424, 12, 8, 2, {}, {}, {{1, false, {168.683, 383.618}, {168.683, 385.230}}}},
        {425, 7, 10, 2, {}, {}, {{1, false, {59.423, 251.113}, {59.423, 252.615}}}},
        {433, 4, 2, 1, {}, {}, {{1, false, {255.473, 169.659}, {255.473, 170.821}}}},
        // A terminal shoulder must leave space for connecting ink after a matched medial body.
        {481, 5, 8, 1, {}, ConnectorWitness{2, {132.10, 198.22}, 3.956}},
        {522, 2, 1, 1, {}, ConnectorWitness{2, {326.95, 116.94}, 1.139}},
        {537, 2, 1, 2, {}, ConnectorWitness{4, {260.15, 116.89}, 3.271},
         {{3, false, {269.056, 116.754}, {269.056, 118.126}}}},
        // A successful generic walk must not give the stacked head to lām.
        {134, 10, 4, 2, {{1, {241.775, 319.946}}, {2, {244.212, 329.320}}}},
        {219, 12, 4, 2, {{1, {211.213, 373.192}}, {2, {214.400, 383.316}}}},
        {344, 8, 10, 1, {{1, {137.902, 269.137}}, {2, {139.777, 276.824}}}},
        // Small upward turns near the tip of reh belong to its bowl. Its entry is at the shoulder.
        {180, 7, 5, 2, {{4, {183.28, 252.25}}}, ConnectorWitness{3, {184.53, 252.50}, 3.022}},
        {334, 15, 7, 2, {{2, {113.27, 464.74}}}, ConnectorWitness{1, {114.52, 465.24}, 8.074}},
        // The blade and lower arm belong to kaf. The neighboring ascender belongs to alef.
        {4,
         5,
         8,
         1,
         {{1, {135.32, 194.17}}, {2, {131.42, 187.43}}, {2, {129.35, 195.24}}, {3, {124.46, 188.56}}}},
        // The separate sloping stroke belongs entirely to alef. The shaft and bowl belong to lam.
        {243,
         15,
         5,
         1,
         {{2, {200.57, 458.72}}, {2, {199.22, 466.06}}, {3, {194.23, 459.37}}, {3, {198.91, 463.06}}}},
        // Both lips of jim, then the shaft of final kaf.
        {424, 9, 4, 4, {{1, {258.71, 302.16}}, {1, {255.47, 304.57}}, {2, {253.18, 296.19}}}},
        // Hanging kaf ends above the loop. Meem owns the loop and the tail.
        {7,
         5,
         2,
         2,
         {{1, {246.27, 184.48}}, {1, {242.04, 192.14}}, {2, {241.15, 197.55}}, {2, {237.55, 201.30}}}},
        // The enclosed jim keeps its upper and lower lips.
        {6,
         9,
         8,
         2,
         {{1, {142.03, 303.44}}, {2, {128.60, 303.70}}, {2, {126.71, 306.38}}, {3, {123.86, 301.42}}}},
        // A detached incoming stroke and the head on the second contour share one medial letter.
        {203, 7, 8, 1, {{3, {57.61, 252.47}}, {3, {51.49, 249.08}}}},
        {219, 6, 5, 1, {{3, {171.89, 225.50}}, {3, {165.30, 221.62}}}},
        // Consecutive jim and hah each own their open head.
        {449, 11, 2, 2, {{2, {273.57, 356.02}}, {3, {264.07, 355.22}}}},
        // The two ink bridges between kaf and alef follow the slightly bent shaft.
        {30, 14, 6, 1, {{1, {225.71, 436.83}}, {2, {224.85, 440.40}}}},
        // The entry stops half a unit from lam's fork. Its shaft and lower
        // return stay with lam; the former long approach is connecting ink.
        {284, 6, 5, 2, {{2, {222.93, 222.91}}, {3, {217.01, 216.11}}},
                         ConnectorWitness{1, {225.21, 222.55}, 3.541}},
        {579, 7, 9, 2, {{3, {66.60, 246.65}}, {4, {59.61, 242.39}}},
                         ConnectorWitness{2, {67.37, 248.95}, 4.785}},
        // A local fork refinement must keep the partition complete. If it
        // does not fit, the previously valid partition must still recover.
        {69, 15, 6, 1, {}},
        {435, 11, 6, 2, {}},
        // The short yeh tooth must not take the preceding lam's tall shaft.
        {561, 6, 7, 1, {{2, {68.8, 218.0}}, {3, {66.7, 223.4}}}},
        // A long diagonal exit must not keep the fork outside the search.
        {95, 3, 8, 1, {}},
        // The folded left flank briefly returns rightward before it descends.
        {79, 12, 2, 2, {}},
        // A tapered connection to hah must not hide the lam's fork.
        {399, 10, 4, 2, {}},
        {35, 3, 1, 2, {}},
        {12, 10, 7, 2, {}},
        {101, 12, 4, 3, {}},
        // Both chords can be short while their anchors are far from a folded foot.
        {36, 15, 4, 2, {}},
        // A steep flank can lean, and a low join can put the tip beyond three stroke widths.
        {29, 7, 4, 1, {}},
        {36, 13, 2, 2, {}},
        {521, 12, 1, 2, {}},
        // Both half-unit walks fit once the adjacent seam is resolved.
        {140, 12, 9, 2, {}},
        {184, 14, 10, 1, {}},
        {415, 14, 3, 1, {}},
        {429, 3, 2, 1, {}},
        // A tapered incoming connector must not cap the width of the fork cut.
        {235, 5, 7, 1, {}},
        // The descending arm of stacked ain, the third seen tooth and the
        // low returning arm of ain end at three different medial jim entries.
        {208, 3, 9, 1, {{1, {119.15, 140.5}}, {2, {120.15, 142.55}}, {2, {115.7, 144.2}}}},
        {341, 8, 5, 3, {{1, {124.9, 277.7}}, {1, {124.6, 278.8}}, {2, {112, 276.9}}, {2, {111.8, 279.8}}}},
        {318, 1, 3, 1, {{1, {280, 90.8}}, {2, {273, 87.3}}, {2, {271.1, 90.7}}}},
        // The medial ain keeps its own head. The low stroke between its
        // left edge and the entry of jim is a separate connector.
        {230, 2, 5, 2, {{2, {228.42, 114.62}}, {3, {220.02, 114.25}}},
         ConnectorWitness{2, {224.19, 116.10}, 3.521}},
        {229, 12, 8, 1, {{2, {148.60, 383.74}}, {3, {139.81, 383.19}}},
         ConnectorWitness{2, {144.60, 385.18}, 2.902}},
        {267, 9, 5, 1, {{4, {206.96, 303.02}}, {5, {198.51, 302.53}}},
         ConnectorWitness{4, {202.99, 304.24}, 3.197}},
        // An overlong connector cannot take either lip of a medial head.
        {572, 13, 12, 1, {{2, {120.25, 410.64}}, {2, {120.25, 412.58}}, {3, {110.53, 409.76}},
                          {3, {110.53, 411.76}}, {4, {100.29, 414.48}}}},
        {184, 10, 8, 1, {{2, {143.08, 329.21}}, {3, {133.79, 328.59}}, {3, {133.79, 330.35}},
                         {4, {124.11, 332.93}}}},
        // Both arms of the initial ain stay together, including before a lam.
        {240, 14, 2, 1, {{1, {308.07, 433.02}}, {1, {308.07, 437.56}}, {2, {298.88, 435.33}},
                         {2, {298.88, 437.11}}}},
        {224, 10, 8, 1, {{1, {205.18, 326.59}}, {1, {205.18, 330.93}}}},
        // The kha head was in its incoming connector. Final ain keeps its own head and bowl.
        {294, 2, 8, 1, {{2, {133.74, 110.36}}, {2, {133.74, 112.36}}, {3, {128.67, 111.25}},
                        {3, {128.67, 123.21}}}},
        // Hah already has its mouth. The folded upright and bowl in the connector belong to noon.
        {515, 13, 8, 2, {{3, {59, 412.38}}, {3, {59, 414.09}}, {4, {52.72, 413.41}},
                         {4, {52.72, 418.94}}}},
        // A long smooth connector from initial ain is genuine.
        {28, 14, 9, 2, {}},
        {124, 9, 2, 2, {{2, {304.53, 301.99}}, {2, {307.27, 302.93}}}},
        {274, 13, 2, 1, {{2, {310.93, 410.72}}, {3, {301.53, 409.54}}}},
        {509, 2, 1, 1, {{2, {276.78, 114.24}}}},
        // The descending blade stays with kaf beside an upright lam.
        {372, 9, 9, 2, {{3, {123.39, 303.68}}}},
        // A tight head-to-upright seam keeps the jim lip and the full lam shaft.
        {47, 3, 3, 3, {{1, {227.95, 142.4}}, {2, {222.21, 138}}, {2, {220.35, 148.8}}}},
        // Three tips and the terminal bowl belong to seen, across a long connector from yeh.
        {246, 2, 6, 2, {{2, {222.15, 115.56}}, {2, {219.84, 115.56}}, {2, {215.14, 115.56}},
                       {2, {211.78, 122.6}}, {1, {229.41, 115.56}}}},
        {71, 2, 10, 1, {{1, {83.03, 114.5}}, {2, {76.69, 114.5}}, {2, {76.05, 120.2}}}},
        // Touching contours of the folded lam-alif retain distinct ink ownership.
        {252, 5, 7, 2, {{3, {145.33, 191}}, {2, {151.56, 191}}, {1, {157.29, 194}}}},
        {384, 4, 6, 2, {{1, {238.27, 166.5}}, {2, {226.33, 166.5}}, {2, {230.04, 170.4}}}},
        // The two bridges of kaf join the upright separately around the open counter.
        {442, 5, 7, 1, {{1, {217.97, 187}}, {2, {211.42, 193.5}}, {1, {213.35, 193.5}}, {2, {210.02, 201.6}}}},
        {91, 10, 6, 1, {}},
        // The shaft and enclosed head of medial tah belong to the same body.
        {89, 9, 10, 1, {{4, {62.24, 295.48}}, {4, {64.36, 304.61}}}},
        // The first and last strokes of final meem surround its own counter.
        {525, 12, 10, 1, {{2, {99.77, 384.44}}, {2, {97.03, 388.93}}}},
        // The return and the bottom of the folded yeh bowl must both survive.
        {54, 8, 9, 2, {{2, {147.51, 278.11}}, {2, {142.72, 280.12}}}},
        {304, 9, 10, 4, {{1, {124.18, 300.82}}, {1, {125.33, 304.99}}}},
        // A later shared hah boundary must leave the cap and shaft on initial lam.
        {94, 3, 8, 2, {{1, {163.83, 134.34}}, {1, {164.27, 136.56}}}},
        // A low inward turn must not hide the fork belonging to the real shaft.
        {163, 15, 8, 2, {}},
        // A subdivided third crest still owns its descending flank.
        {285, 10, 5, 2, {}},
        // The neck below the two eyes is the heh exit.
        {413, 8, 2, 1, {}},
        // A normal just before the outer turn must end on that turn.
        {537, 5, 3, 2, {}},
        // Final ain keeps both the raised head and the descending bowl.
        {76, 1, 6, 2, {{3, {188, 88.5}}, {3, {190.7, 101.27}}}},
        {597, 7, 4, 2, {{1, {207.15, 243.48}}, {1, {205.1, 245.22}}}},
        // The shaft stops above the projecting head, even when a later kaf is taller.
        {20, 6, 5, 2, {{1, {210.76, 216.47}}, {2, {215.2, 222.13}}, {2, {207.60, 224.33}}}},
        // The approach to the fold belongs to ghain. The return and bowl belong to yeh.
        {131, 13, 9, 1, {{4, {51.5, 414.5}}, {5, {53.5, 415.5}}, {5, {51.0, 418.0}}}},
        // Seen and heh sit on separate outlines but remain medial in the transcript.
        {189, 15, 1, 3, {{3, {264.64, 465.5}}, {4, {255.67, 459.97}}, {5, {251.33, 467.63}}}},
        // The ascending lam and the kaf blade touch at two bridges.
        {403, 13, 6, 1, {{1, {184.27, 408.91}}, {2, {179.34, 402.07}}, {2, {175.21, 409.35}}}},
        {437, 1, 7, 2, {{2, {127.78, 84.91}}, {2, {127.78, 87.8}}}},
        // The final meem opens back into the connector. Stop on its first visible rim.
        {579, 10, 5, 1, {{2, {179.3, 324.96}}, {3, {176.95, 329.24}}, {4, {171.33, 324.54}},
                         {5, {167.43, 331.44}}}},
        // Only the descending lam is above the heh. The folded kaf owns its blade.
        {600, 13, 1, 2, {{1, {330.56, 399.88}}, {2, {331.22, 408.06}}, {3, {325.31, 409.61}},
                         {4, {317.82, 399.93}}, {5, {311.78, 415.6}}}},
        // The khāʾ loop ends before either lām shaft.
        {194, 14, 6, 1, {{1, {172.20, 439.70}}, {2, {168.45, 431.00}}, {3, {162.32, 431.25}}}},
        // The final tāʾ keeps its right tip, and the short approach remains a connector.
        {565, 2, 11, 2, {{4, {53.80, 114.26}}}, ConnectorWitness{3, {54.20, 115.15}, .559}},
        // The entry at the lower rim keeps the ḍād's eye and exposes the preceding connector.
        {560, 6, 10, 1, {{3, {121.02, 225.22}}, {3, {122.50, 223.15}}},
                         ConnectorWitness{2, {122.40, 225.05}, 2.433}},
        // The lower lip of jīm runs almost to hāʾ; only the short join is separate.
        {579, 9, 8, 3, {{1, {128.40, 303.00}}}, ConnectorWitness{1, {126.54, 303.72}, .421}},
    };
    for (const auto& e : examples) {
        const auto id = QString("page%1:l%2-w%3-s%4").arg(e.page).arg(e.line).arg(e.word).arg(e.subword);
        QFile pageFile(QString("pages/page%1.dat").arg(e.page));
        require(pageFile.open(QIODevice::ReadOnly), id + " missing page");
        PageAnalysisResult a;
        QDataStream stream(&pageFile);
        stream >> a.page;
        a.resolveWordBoundaries(e.page, &font);
        auto& word = a.page.lines[e.line - 1].words[e.word - 1];
        if (word.wordResultInfo.subWords.empty())
            a.detectSubWords(e.page - 1, e.line - 1, e.word - 1, &font);
        require(a.segmentSubword(e.page - 1, e.line - 1, e.word - 1, e.subword - 1, &font, true),
                id + " incomplete");
        const auto& sub = word.wordResultInfo.subWords[e.subword - 1];
        const auto shape = word.bodyShape(sub);
        if (e.page == 220)
            require(sub.kashidaPaths.size() == 3 && sub.kashidaPaths.back().isEmpty(),
                    id + " expected a shared fold between jim and yeh");
        if (e.page == 286 || e.page == 154)
            require(sub.kashidaPaths.size() + 1 == sub.baseGlyphs.size() && sub.kashidaPaths.back().isEmpty(),
                    id + " expected a shared lam and yeh fold");
        if ((e.page == 12 && e.line == 10 && e.word == 7 && e.subword == 2) ||
            (e.page == 101 && e.line == 12 && e.word == 4 && e.subword == 3)) {
            const int lam = e.page == 12 ? 2 : 1;
            require(sub.letterDiagnostics[lam].toObject().value("boundaryKind") == "lam-fork" &&
                    sub.kashidaCuts[lam - 1].second.length() < 1.3 &&
                    sub.kashidaCuts[lam].first.length() < 1.3 &&
                    !sub.kashidaPaths[lam].isEmpty(),
                    id + " expected primary fork cuts and a separate connector into hah");
        }
        if (e.page == 95 && e.line == 3 && e.word == 8 && e.subword == 1) {
            require(sub.kashidaCuts.size() == 5 && sub.kashidaCuts[3].second.length() < 1.4 &&
                    sub.kashidaCuts[4].first.length() < 1.4,
                    id + " expected two local lam cuts instead of a diagonal through the foot");
        }
        if (e.page == 79 && e.line == 12 && e.word == 2 && e.subword == 2) {
            require(sub.kashidaCuts.size() == 2 &&
                    QLineF(sub.kashidaCuts[0].second.p2(), {210.609, 384.692}).length() < .01 &&
                    QLineF(sub.kashidaCuts[1].first.p2(), {210.280, 384.822}).length() < .01,
                    id + " expected both anchors half a page unit along the folded contour");
        }
        if (e.page == 399 && e.line == 10 && e.word == 4 && e.subword == 2) {
            require(sub.kashidaCuts.size() == 5 && sub.kashidaCuts[1].second.length() < 1.3 &&
                    sub.kashidaCuts[2].first.length() < 1.3 && !sub.kashidaPaths[2].isEmpty(),
                    id + " expected local lam cuts with a separate connector to hah");
        }
        if (e.page == 35) {
            require(sub.kashidaCuts.size() == 4 && sub.kashidaCuts[2].second.length() < 1.3 &&
                    sub.kashidaCuts[3].first.length() < 1.3 && !sub.kashidaPaths[3].isEmpty(),
                    id + " expected local lam cuts before final hah");
        }
        if (e.page == 29 || e.page == 36 || e.page == 521) {
            const int lam = e.page == 36 && e.line == 13 ? 2 : 1;
            const QPointF tip = e.page == 29 ? QPointF{186.953, 251.150} :
                e.page == 521 ? QPointF{278.708, 384.631} :
                lam == 2 ? QPointF{307.940, 409.680} : QPointF{254.816, 467.331};
            require(sub.kashidaCuts.size() > size_t(lam), id + " expected two connections to lam");
            const auto right = sub.kashidaCuts[lam - 1].second.p2();
            const auto left = sub.kashidaCuts[lam].first.p2();
            const double rightReach = QLineF(tip, right).length(), leftReach = QLineF(tip, left).length();
            require(rightReach > .45 && rightReach < .51 && leftReach > .45 && leftReach < .51 &&
                    QLineF(right, left).length() > .1,
                    id + " expected separate contour anchors near the folded foot");
        }
        const auto expectHalfWalk = [&](size_t lam, const QPointF& right, const QPointF& left) {
            require(sub.kashidaCuts.size() > lam && !sub.kashidaPaths[lam - 1].isEmpty() &&
                    !sub.kashidaPaths[lam].isEmpty() &&
                    QLineF(sub.kashidaCuts[lam - 1].second.p2(), right).length() < .01 &&
                    QLineF(sub.kashidaCuts[lam].first.p2(), left).length() < .01,
                    id + " expected both half-unit cuts and separate adjacent connectors");
        };
        if (e.page == 140) expectHalfWalk(1, {116.150, 384.988}, {115.813, 384.971});
        if (e.page == 163) expectHalfWalk(2, {82.075, 462.562}, {81.787, 462.523});
        if (e.page == 184 && e.line == 14) {
            expectHalfWalk(3, {99.465, 436.310}, {99.260, 436.293});
            require(sub.letterDiagnostics.last().toObject().value("template") == "meem-shoulder-fina",
                    id + " expected the final meem to keep its shoulder");
        }
        if (e.page == 235 && e.line == 5) {
            expectHalfWalk(1, {116.390, 197.172}, {116.077, 197.273});
            require(sub.kashidaCuts[0].second.length() > 2.5 * sub.kashidaCuts[0].first.length(),
                    id + " expected the fork cut to survive the tapered connector");
        }
        if (e.page == 415) expectHalfWalk(1, {250.382, 440.019}, {249.956, 439.969});
        if (e.page == 429) expectHalfWalk(1, {266.492, 146.112}, {266.155, 146.093});
        if (e.page == 69) {
            require(sub.baseGlyphs.size() == 5 && sub.kashidaCuts.size() == 4,
                    id + " expected five complete letter bodies");
            const auto exit = sub.kashidaCuts.back().first;
            require(exit.length() > 0 && exit.length() < 1.5 && sub.kashidaCuts[2].second.length() < 1.5,
                    id + " expected short local cuts around the lam fork");
        }
        if (e.page == 435) {
            require(sub.baseGlyphs.size() == 4 && sub.kashidaCuts.size() == 3,
                    id + " expected four complete letter bodies and three connectors");
            const auto exit = sub.kashidaCuts.back().first;
            require(exit.length() > 0 && exit.length() < 1.3 && sub.kashidaCuts[1].second.length() < 1.3 &&
                    !sub.kashidaPaths.back().isEmpty(), id + " expected local lam cuts with the final connector intact");
        }
        for (const auto& witness : e.witnesses) {
            const auto local = shape.transform.inverted().map(witness.point - shape.pos);
            require(shape.path.contains(local), id + " misplaced test point");
            for (size_t slot = 0; slot < sub.baseGlyphs.size(); ++slot)
                require(sub.baseGlyphs[slot].path.contains(local) == (slot + 1 == witness.letter),
                        id + QString(" wrong owner at (%1,%2): expected letter %3, tested letter %4")
                            .arg(witness.point.x()).arg(witness.point.y()).arg(witness.letter).arg(slot + 1));
            for (const auto& connector : sub.kashidaPaths)
                require(!connector.contains(local), id + " letter stroke in kashida");
        }
        for (const auto& cap : e.caps) {
            const auto slot = cap.afterLetter - 1;
            require(slot < sub.kashidaPaths.size() && !sub.kashidaPaths[slot].isEmpty(),
                    id + " expected a separate connector");
            const auto& [sending, receiving] = sub.kashidaCuts[slot];
            const auto& line = cap.receiving ? receiving : sending;
            require(QLineF(line.p1(), cap.top).length() < .01 && QLineF(line.p2(), cap.bottom).length() < .01,
                    id + " expected the cut at the reviewed stroke section");
        }
        if (e.connector) {
            const auto slot = e.connector->afterLetter - 1;
            const auto local = shape.transform.inverted().map(e.connector->ink - shape.pos);
            require(slot < sub.kashidaPaths.size() && sub.kashidaPaths[slot].contains(local),
                    id + " missing connecting ink");
            for (const auto& body : sub.baseGlyphs)
                require(!body.path.contains(local), id + " connecting ink also assigned to a letter");
            const auto& [right, left] = sub.kashidaCuts.at(slot);
            require(std::abs(right.center().x() - left.center().x() - e.connector->horizontalSpan) < .01,
                    id + " incorrect distance between the connector cuts");
        }
        if (e.page == 47 || e.page == 71 || e.page == 442)
            require(sub.kashidaPaths.size() == 1 && sub.kashidaPaths[0].isEmpty(), id + " expected a tight attachment");
        if (e.page == 208 || e.page == 318 || e.page == 341) {
            require(sub.kashidaPaths.size() >= 2 && !sub.kashidaPaths[1].isEmpty(), id + " missing lip exit");
            const double tipX = e.page == 208 ? 115.810 : e.page == 318 ? 271.199 : 111.061;
            require(std::abs(sub.kashidaCuts[1].first.center().x() - (tipX - .25)) < .03,
                    id + " outgoing cut missed the free lip");
            if (e.page == 341) {
                const auto local = shape.transform.inverted().map(QPointF(121,280.2) - shape.pos);
                require(sub.kashidaPaths[0].contains(local), id + " missing connector after the third tooth");
            } else require(sub.kashidaPaths[0].isEmpty(), id + " expected a shared ain-jim cut");
        }
        if (e.page == 28) {
            require(sub.kashidaPaths.size() == 1 && !sub.kashidaPaths.front().isEmpty(), id + " lost the long connector");
            const auto& [start, end] = sub.kashidaCuts.front();
            require(start.center().x() - end.center().x() >= 14.165, id + " genuine kashida shortened");
        }
        if (e.page == 20 || e.page == 134 || (e.page == 219 && e.line == 12) || e.page == 344)
            require(sub.letterDiagnostics.first().toObject().value("template") == "lam-hah-stacked-init" &&
                    !sub.kashidaPaths.empty() && sub.kashidaPaths.front().isEmpty(), id + " expected stacked lam");
        if (e.page == 131) {
            require(sub.baseGlyphs.size() == 5 && sub.joins.size() == 4 && sub.kashidaPaths.back().isEmpty(),
                    id + " expected one shared ghain–yeh cut");
            const auto& cut = *sub.joins.rbegin();
            require(std::abs(cut.topBaseLine.time - 15.552) < .01 && std::abs(cut.bottomBaseLine.time - 25) < .01,
                    id + " cut missed the first turn of the bowl");
        }
        if (e.page == 189) {
            const auto seen = sub.letterDiagnostics[2].toObject(), heh = sub.letterDiagnostics[3].toObject();
            require(seen.value("position") == "medial" && heh.value("position") == "medial" &&
                    heh.value("boundaryKind") == "separate-contours" && sub.joins.size() == 3,
                    id + " incorrect cut at the pen lift");
        }
        if (e.page == 403)
            require(sub.joins.rbegin()->cutSegments().size() >= 2, id + " missing kaf bridge");
        if (e.page == 285) {
            require(sub.kashidaCuts.size() == 1 && !sub.kashidaPaths.front().isEmpty() &&
                    QLineF(sub.kashidaCuts.front().first.p2(), {191.259, 332.450}).length() < .01,
                    id + " expected the seen exit on the opposing bend below the third tooth");
        }
        if (e.page == 413) {
            require(sub.baseGlyphs.size() == 7 && sub.baseGlyphs[5].path.toSubpathPolygons().size() == 3 &&
                    sub.kashidaCuts.size() == 6 && sub.kashidaCuts[5].first.p1().y() > 274.621 &&
                    sub.kashidaCuts[5].first.length() < 1,
                    id + " expected both heh counters and a short exit below them");
        }
        if (e.page == 537 && e.line == 5) {
            require(sub.kashidaCuts.size() == 2 && !sub.kashidaPaths.front().isEmpty() &&
                    QLineF(sub.kashidaCuts.front().first.p2(), {243.779, 195.269}).length() < .01,
                    id + " expected the initial lam to retain its rightmost turn");
        }
        if (e.page == 437) {
            const auto hole = shape.transform.inverted().map(QPointF(129.8, 89.0) - shape.pos);
            require(!shape.path.contains(hole), id + " expected the source counter");
            for (const auto& body : sub.baseGlyphs)
                require(!body.path.contains(hole), id + " filled the counter of zah");
        }
        if (e.page == 579 && e.line == 10) {
            require(sub.letterDiagnostics[1].toObject().value("boundaryKind") == "lam-fork",
                    id + " expected the primary lam fork to compose with final meem");
            require(!sub.kashidaPaths.empty() && !sub.kashidaPaths.back().isEmpty(), id + " lost the final connector");
            const auto edge = sub.kashidaCuts.back().second;
            require(QLineF(edge.p1(), {167.875, 330.173}).length() < .01 &&
                    QLineF(edge.p2(), {169.033, 329.121}).length() < .01, id + " crossed the opening in meem");
        }
        if (e.page == 246) {
            require(sub.kashidaCuts.size() == 1 && !sub.kashidaPaths[0].isEmpty(), id + " lost the connector");
            const auto entry = sub.kashidaCuts[0].second;
            require(QLineF(entry.p1(), {222.135, 116.035}).length() < .02 &&
                    QLineF(entry.p2(), {221.783, 116.807}).length() < .02, id + " cut moved away from first tooth");
        }
        if (e.page == 30) {
            require(sub.joins.size() == 1 && sub.joins.begin()->cutSegments().size() == 3,
                    id + " expected the bent upper bridge and straight lower bridge");
        }
        if (e.page == 203 || (e.page == 219 && e.line == 6)) {
            require(sub.letterDiagnostics[2].toObject().value("template") == "hah-overhanging-entry-medi",
                    id + " expected a joined attachment and head");
        }
        if (e.page == 243) {
            require(sub.letterDiagnostics.last().toObject().value("boundaryKind") == "separate-contours",
                    id + " expected a separate alef component");
        }
        if (e.page == 91) {
            require(sub.baseGlyphs.size() == 4 && sub.joins.size() == 3,
                    id + " expected four letters with three native joins");
            const auto& bridge = *std::next(sub.joins.begin());
            require(bridge.counterEntry.pathIndex > 0 && bridge.counterExit.pathIndex == bridge.counterEntry.pathIndex &&
                    bridge.cutSegments().size() >= 2,
                    id + " expected both bridges of the original kaf–lam counter");
        }
        if (e.page == 89) {
            auto config = PageAnalysisResult::letterForms();
            config.insert("retryUnresolvedForms", false);
            require(!a.segmentSubword(e.page - 1, e.line - 1, e.word - 1, e.subword - 1, &font, true, &config),
                    id + " recovery-only forms ran with recovery disabled");
        }
    }
    qInfo("Native letter bodies and their shared or separate attachment cuts passed the ink ownership checks.");
    return 0;
}
