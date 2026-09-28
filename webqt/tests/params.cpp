#include "Params.h"
#include <QCoreApplication>
#include <iostream>
#include <cstdlib>
void require(bool ok,const char *message){if(!ok){std::cerr<<message<<'\n';std::exit(1);}}
int main(int argc,char **argv){
 QCoreApplication app(argc,argv);
 for(int mode=0;mode<3;++mode){
  QJsonObject expected;
  for(const auto &d:teedsp::kParams) expected[QString::number(d.id)]=mode==0?d.minVal:mode==1?d.maxVal:d.defVal;
  auto actual=remote::encode(remote::decode(expected));
  require(actual.size()==teedsp::kParamCount,"all 69 parameters must round-trip");
  for(const auto &d:teedsp::kParams){
   const auto key=QString::number(d.id);
   require(std::abs(actual[key].toDouble()-expected[key].toDouble())<1e-5,"parameter ID/field mapping mismatch");
  }
 }
 auto p=dsp::ChainParams{};p.eqBands[2].q=500;p.eqBands[4].gainDb=-100;
 auto clamped=remote::encode(p);
 require(clamped["123"].toDouble()==20,"wheel Q must respect service bounds");
 require(clamped["144"].toDouble()==-24,"EQ gain must respect service bounds");
 std::cout<<"PASS all parameter mappings at defaults and limits, gesture clamping\n";
}
