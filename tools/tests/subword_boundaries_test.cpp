#include "pageanalysisresult.h"
#include "afont.h"
#include "GlobalParams.h"
#include <QApplication>
#include <QFile>
#include <QJsonDocument>

static void check(bool ok,const QString& message){if(!ok)qFatal("%s",qPrintable(message));}
static QByteArray pageBytes(const APage& page){QByteArray b;QDataStream s(&b,QIODevice::WriteOnly);s<<page;return b;}
static QByteArray pathsBytes(const Word& word){
 auto paths=word.paths;
 // Stop-sign type restoration is checked below; source geometry stays exact.
 for(auto& p:paths)p.type=CustomDataType(0);
 QByteArray b;QDataStream s(&b,QIODevice::WriteOnly);s<<paths;return b;
}
static int contours(const Word& word,const WordResultInfo::SubWordInfo& sub){int n=0;for(int pi:sub.paths)n+=1+word.paths[pi].nbpath;return n;}
static PageAnalysisResult readPage(int number){QFile f(QString("pages/page%1.dat").arg(number));check(f.open(QIODevice::ReadOnly),"Missing native cache");PageAnalysisResult a;QDataStream s(&f);s>>a.page;check(s.status()==QDataStream::Ok,"Invalid native cache");return a;}
static QRectF box(const Apath& p){auto q=p.transform.map(p.path);q.translate(p.pos);return q.boundingRect();}
int main(int argc,char**argv){
 QApplication app(argc,argv);Q_INIT_RESOURCE(application);globalParams=std::make_unique<GlobalParams>();PageAnalysisResult::initQuranText();
 AFont font;QFile f("input/font.dat");check(f.open(QIODevice::ReadOnly),"Missing native font");QDataStream fs(&f);fs>>font;
 QFile fixtureFile("tools/tests/fixtures/subword-boundary-regressions.json");check(fixtureFile.open(QIODevice::ReadOnly),"Missing subword regression fixture");
 const auto fixtures=QJsonDocument::fromJson(fixtureFile.readAll()).array();QSet<QString> targets;QMap<int,QJsonArray> byPage;
 for(auto value:fixtures){auto r=value.toObject();targets.insert(r["id"].toString());byPage[r["page"].toInt()].append(r);}
 QJsonArray changes,repairs;QSet<QString> changedWords;int pathsChecked=0,ownersChecked=0;
 for(int pn=1;pn<=604;++pn){auto a=readPage(pn);auto before=a.page;
  for(int li=0;li<a.page.lines.size();++li)for(int wi=0;wi<a.page.lines[li].words.size();++wi){
   a.detectSubWords(pn-1,li,wi,&font);const auto& old=before.lines[li].words[wi];const auto& word=a.page.lines[li].words[wi];
   check(pathsBytes(old)==pathsBytes(word),"A source path, transform, contour or word owner changed");
   for(int pi=0;pi<old.paths.size();++pi)if(old.paths[pi].type!=word.paths[pi].type){
    const auto& p=word.paths[pi];
    check(int(old.paths[pi].type)==0 && p.type==CustomDataType::WAQF && p.nbpath==0 &&
      (font.checkGlyph(p.path,"sad.waqf")!=-1 || font.checkGlyphScaled(p.path,"sad.waqf")!=-1),
      "Unexpected native path type change");
   }
   check(old.wordResultInfo.subWords.size()==word.wordResultInfo.subWords.size(),"Subword text boundary changed");
   pathsChecked+=word.paths.size();
   for(int si=0;si<int(word.wordResultInfo.subWords.size());++si){const auto& x=old.wordResultInfo.subWords[si];const auto& y=word.wordResultInfo.subWords[si];
    check(x.text==y.text && contours(old,x)==contours(word,y),"Subword text or contour quota changed");
    if(!x.paths.empty())check(x.paths.front()==y.paths.front(),"Subword body changed");
    auto p=x.paths,q=y.paths;std::sort(p.begin(),p.end());std::sort(q.begin(),q.end());
    if(p!=q){const auto id=QString("page%1:l%2-w%3").arg(pn,3,10,QChar('0')).arg(li+1).arg(wi+1);changedWords.insert(id);QJsonArray from,to;for(int pi:x.paths)from.append(pi);for(int pi:y.paths)to.append(pi);changes.append(QJsonObject{{"word",id},{"subword",si+1},{"text",y.text},{"before",from},{"after",to}});}
   }
  }
  for(auto value:byPage.value(pn)){auto r=value.toObject();const auto& w=a.page.lines[r["line"].toInt()-1].words[r["word"].toInt()-1];
   for(const auto item:{qMakePair(QString("boxToLeft"),r["leftSubword"].toInt()-1),qMakePair(QString("boxToRight"),r["rightSubword"].toInt()-1)}){
    auto b=r[item.first].toArray();QRectF expected(b[0].toDouble(),b[1].toDouble(),b[2].toDouble(),b[3].toDouble());bool found=false;
    for(int pi:w.wordResultInfo.subWords[item.second].paths){auto actual=box(w.paths[pi]);if(QLineF(expected.topLeft(),actual.topLeft()).length()<.01&&QLineF(expected.bottomRight(),actual.bottomRight()).length()<.01)found=true;}
    check(found,r["id"].toString()+": expected source mark has wrong subword owner");++ownersChecked;
   }
  }
  const auto stable=pageBytes(a.page);const int count=a.subwordBoundaryRepairs.size();
  for(int li=0;li<a.page.lines.size();++li)for(int wi=0;wi<a.page.lines[li].words.size();++wi)a.detectSubWords(pn-1,li,wi,&font);
  check(pageBytes(a.page)==stable && a.subwordBoundaryRepairs.size()==count,"Subword repair is not idempotent");
  for(auto r:a.subwordBoundaryRepairs)repairs.append(r);
  if(pn%100==0)qInfo()<<"Checked native subword pages:"<<pn;
 }
 QFile output("/private/tmp/subword-native-audit.json");output.open(QIODevice::WriteOnly);output.write(QJsonDocument(QJsonObject{{"pages",604},{"pathsChecked",pathsChecked},{"changedWords",int(changedWords.size())},{"changes",changes},{"repairs",repairs}}).toJson());
 for(const auto& id:changedWords)check(targets.contains(id),"Unexpected native ownership change: "+id);
 check(ownersChecked==fixtures.size()*2,"Not all expected corrected marks were checked");
 qInfo()<<"Native changed words:"<<changedWords.size()<<"exchanges:"<<repairs.size();
 // Exercise source-PDF parsing for every recurring boundary pattern.
 for(int pn:byPage.keys()){
  auto cached=readPage(pn);cached.resolveWordBoundaries(pn,&font);
  PageAnalysisResult fresh;check(fresh.loadPage(pn,&font,false)==0,"Fresh PDF segmentation failed");
  for(auto r:byPage[pn]){auto rec=r.toObject();int li=rec["line"].toInt()-1,wi=rec["word"].toInt()-1;
   const auto& expected=cached.page.lines[li].words[wi];const auto& actual=fresh.page.lines[li].words[wi];
   for(int si=0;si<int(expected.wordResultInfo.subWords.size());++si)for(int pi:expected.wordResultInfo.subWords[si].paths){const auto b=box(expected.paths[pi]);bool found=false;
    for(int pj:actual.wordResultInfo.subWords[si].paths){auto c=box(actual.paths[pj]);if(QLineF(b.topLeft(),c.topLeft()).length()<.01&&QLineF(b.bottomRight(),c.bottomRight()).length()<.01)found=true;}
    check(found,QString("Fresh PDF ownership differs on page %1").arg(pn));
   }
  }
 }
 // Ambiguous and invalid-quota inputs must stay unresolved.
 {
  auto r=fixtures[0].toObject();int pn=r["page"].toInt(),li=r["line"].toInt()-1,wi=r["word"].toInt()-1;
  auto ambiguous=readPage(pn);auto& w=ambiguous.page.lines[li].words[wi];int original=r["pathToRight"].toInt(),duplicate=w.paths.size();w.paths.append(w.paths[original]);w.wordResultInfo.subWords[r["leftSubword"].toInt()-1].paths.push_back(duplicate);
  const auto before=w.wordResultInfo;ambiguous.detectSubWords(pn-1,li,wi,&font);
  for(int si=0;si<int(before.subWords.size());++si)check(before.subWords[si].paths==w.wordResultInfo.subWords[si].paths,"Ambiguous candidates were exchanged");
  auto invalid=readPage(pn);auto& bad=invalid.page.lines[li].words[wi];bad.wordResultInfo.state|=WordResultFlags::DIFFERENT_PATH_NUMBER;const auto untrusted=bad.wordResultInfo;invalid.detectSubWords(pn-1,li,wi,&font);
  for(int si=0;si<int(untrusted.subWords.size());++si)check(untrusted.subWords[si].paths==bad.wordResultInfo.subWords[si].paths,"Invalid contour quota justified an exchange");
  auto moved=readPage(pn);auto saved=PageAnalysisResult::QuranTextByWord[2];PageAnalysisResult::QuranTextByWord[2]=PageAnalysisResult::QuranTextByWord[pn-1];
  for(auto& line:moved.page.lines){line.baseline=line.baseline*1.7+31;for(auto& word:line.words)for(auto& path:word.paths){path.transform=path.transform*QTransform::fromScale(1.7,1.7);path.pos=path.pos*1.7+QPointF(17,31);}}
  for(int l=0;l<moved.page.lines.size();++l)for(int w=0;w<moved.page.lines[l].words.size();++w)moved.detectSubWords(2,l,w,&font);
  check(moved.subwordBoundaryRepairs.size()==byPage[pn].size(),"Repair depends on a page ID or absolute position/scale");PageAnalysisResult::QuranTextByWord[2]=saved;
 }
 // A low hamza above the tooth cannot stand in for the neighbour's kasra.
 {
  constexpr int pn=583,li=0,wi=4;auto source=readPage(pn);const auto& original=source.page.lines[li].words[wi];
  check(box(original.paths[6]).center().y()>source.page.lines[li].baseline,"Fixture no longer has a low hamza");
  check(font.checkGlyph(original.paths[6].path,QChar(0x064E),constants::MATCHING_ERROR_RATIO)==-1,"Hamza matched a lower stroke");
  check(font.checkGlyph(original.paths[4].path,QChar(0x064E),constants::MATCHING_ERROR_RATIO)!=-1,"Kasra did not match a lower stroke");
  auto unchanged=[&](PageAnalysisResult a,const QString& reason) {
   const auto old=a.page.lines[li].words[wi].wordResultInfo;
   a.detectSubWords(pn-1,li,wi,&font);
   const auto& now=a.page.lines[li].words[wi].wordResultInfo;
   for(int si=0;si<int(old.subWords.size());++si)check(old.subWords[si].paths==now.subWords[si].paths,reason);
  };
  auto ambiguous=source;auto& w=ambiguous.page.lines[li].words[wi];int duplicate=w.paths.size();
  w.paths.append(w.paths[3]);w.wordResultInfo.subWords[2].paths.push_back(duplicate);
  unchanged(ambiguous,"Ambiguous maddah candidates were exchanged");
  auto nonstroke=source;nonstroke.page.lines[li].words[wi].paths[4].path=original.paths[6].path;
  unchanged(nonstroke,"Unrecognized lower ink satisfied the kasra quota");
  auto distant=source;distant.page.lines[li].words[wi].paths[3].pos.rx()+=50;
  unchanged(distant,"Distant maddah was transferred across subword boundary");
  auto noQuota=source;auto& q=noQuota.page.lines[li].words[wi];
  q.wordResultInfo.subWords[2].text.remove(QChar(0x0650));
  auto savedText=PageAnalysisResult::QuranTextByWord[pn-1][li][wi];
  PageAnalysisResult::QuranTextByWord[pn-1][li][wi].remove(QChar(0x0650));
  unchanged(noQuota,"Missing transcript kasra quota still allowed a transfer");
  PageAnalysisResult::QuranTextByWord[pn-1][li][wi]=savedText;
  auto moved=source;auto savedPage=PageAnalysisResult::QuranTextByWord[2];
  PageAnalysisResult::QuranTextByWord[2]=PageAnalysisResult::QuranTextByWord[pn-1];
  for(auto& line:moved.page.lines){line.baseline=line.baseline*1.7+31;for(auto& word:line.words)for(auto& path:word.paths){path.transform=path.transform*QTransform::fromScale(1.7,1.7);path.pos=path.pos*1.7+QPointF(17,31);}}
  moved.detectSubWords(2,li,wi,&font);
  check(moved.page.lines[li].words[wi].wordResultInfo.subWords[1].paths==std::vector<int>({5,3}),"Maddah repair depends on page ID, position or scale");
  PageAnalysisResult::QuranTextByWord[2]=savedPage;
  source.detectSubWords(pn-1,li,wi,&font);const auto stable=pageBytes(source.page);
  PageAnalysisResult restored;QDataStream input(stable);input>>restored.page;
  restored.detectSubWords(pn-1,li,wi,&font);
  check(pageBytes(restored.page)==stable && restored.subwordBoundaryRepairs.isEmpty(),"Maddah repair changed after serialization");
 }
 qInfo()<<"PASS: all source paths and word owners conserved; subword bodies, text and quotas conserved; idempotent; fresh PDF controls; low-hamza/maddah controls";
}
