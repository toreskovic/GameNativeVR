#pragma once
#include <cstdint>
#include <cstring>
#include <vector>
#include <unordered_map>
#include <unordered_set>

namespace ffr {
// Tint completed location-zero float outputs at fragment entry-point exits.
// DXVK can store individual components through access chains; observing only
// whole-vector stores misses those shaders. Unsupported interfaces stay untouched. No descriptors, push constants or game
// pipeline state change: the driver supplies the real per-invocation footprint.
inline std::vector<uint32_t> fragmentDebug(const uint32_t* code, size_t words) {
  if (words < 5 || code[0] != 0x07230203) return {};
  std::unordered_map<uint32_t, std::vector<uint32_t>> types;
  std::unordered_map<uint32_t, uint32_t> variables;
  std::unordered_set<uint32_t> locationZero, indexed;
  std::unordered_map<uint32_t,uint32_t> components;
  uint32_t entry=0;
  uint32_t boolType=0, intType=0, input=0;
  bool capability=false, extension=false;
  unsigned entries=0;
  for (size_t i=5; i<words;) {
    uint32_t n=code[i]>>16, op=code[i]&65535;
    if (!n || i+n>words) return {};
    if (op==15) { if (n<4 || code[i+1]!=4) return {}; ++entries;entry=code[i+2]; }
    if (op>=19 && op<=33 && n>=2) types[code[i+1]]={code+i,code+i+n};
    if (op==20) boolType=code[i+1];
    if (op==21 && n==4 && code[i+2]==32 && code[i+3]==1) intType=code[i+1];
    if (op==59 && n>=4) variables[code[i+2]]=code[i+1];
    if (op==71 && n>=4) {
      if (code[i+2]==30 && code[i+3]==0) locationZero.insert(code[i+1]);
      if (code[i+2]==31) components[code[i+1]]=code[i+3];
      if (code[i+2]==32 && code[i+3]!=0) indexed.insert(code[i+1]);
      if (code[i+2]==11 && code[i+3]==5292) input=code[i+1];
    }
    if (op==17 && n==2 && code[i+1]==5291) capability=true;
    if (op==10 && n>=2) {
      const char* name=reinterpret_cast<const char*>(code+i+1);
      if (memchr(name,0,(n-1)*4) && !strcmp(name,"SPV_EXT_fragment_invocation_density")) extension=true;
    }
    i+=n;
  }
  if (entries!=1 || input) return {};
  struct Output { uint32_t variable,type,count,component; };
  std::vector<Output> outputs;
  uint32_t fp=0;
  for (auto variable: locationZero) {
    if (indexed.count(variable) || !variables.count(variable)) continue;
    auto ptr=types[variables[variable]];
    if (ptr.size()!=4 || (ptr[0]&65535)!=32 || ptr[2]!=3) continue;
    auto type=types[ptr[3]];
    if (type.empty()) continue;
    uint32_t scalarId=ptr[3],count=1;
    if ((type[0]&65535)==23 && type.size()==4) { scalarId=type[2];count=type[3]; }
    auto scalar=types[scalarId];
    if (scalar.size()!=3 || (scalar[0]&65535)!=22 || scalar[2]!=32) continue;
    uint32_t component=components[variable];
    if (component>=3 || count>4 || component+count>4) continue;
    if (fp && fp!=scalarId) return {};
    fp=scalarId;outputs.push_back({variable,ptr[3],count,component});
  }
  if (outputs.empty()) return {};
  uint32_t next=code[3];
  auto id=[&] { return next++; };
  std::vector<uint32_t> globals, result(code,code+5);
  auto emit=[](std::vector<uint32_t>& dst,uint32_t op,std::initializer_list<uint32_t> args) {
    dst.push_back(uint32_t(args.size()+1)<<16|op);dst.insert(dst.end(),args);
  };
  if (!boolType) { boolType=id();emit(globals,20,{boolType}); }
  if (!intType) { intType=id();emit(globals,21,{intType,32,1}); }
  uint32_t vec2=0,ptr=0;
  for (const auto& t:types) if (t.second.size()==4 && (t.second[0]&65535)==23 && t.second[2]==intType && t.second[3]==2) vec2=t.first;
  if (!vec2) { vec2=id();emit(globals,23,{vec2,intType,2}); }
  for (const auto& t:types) if (t.second.size()==4 && (t.second[0]&65535)==32 && t.second[2]==1 && t.second[3]==vec2) ptr=t.first;
  if (!ptr) { ptr=id();emit(globals,32,{ptr,1,vec2}); }
  input=id();emit(globals,59,{ptr,input,1});
  uint32_t one=id(),two=id(),zeroF=id(),oneF=id(),weight=id(),rest=id();
  emit(globals,43,{intType,one,1});emit(globals,43,{intType,two,2});
  emit(globals,43,{fp,zeroF,0});emit(globals,43,{fp,oneF,0x3f800000});
  emit(globals,43,{fp,weight,0x3eb33333}); // .35
  emit(globals,43,{fp,rest,0x3f266666}); // .65
  bool caps=false,ext=false,annotations=false,declarations=false,changed=false;
  uint32_t function=0;
  for (size_t i=5;i<words;) {
    uint32_t n=code[i]>>16,op=code[i]&65535;
    if (!caps && op!=17) { if (!capability) emit(result,17,{5291});caps=true; }
    if (!ext && op!=17 && op!=10) {
      if (!extension) {
        const char name[]="SPV_EXT_fragment_invocation_density";
        std::vector<uint32_t> bytes((sizeof(name)+3)/4,0);memcpy(bytes.data(),name,sizeof(name));
        result.push_back(uint32_t(bytes.size()+1)<<16|10);result.insert(result.end(),bytes.begin(),bytes.end());
      }
      ext=true;
    }
    if (!annotations && op>=19 && op<=39) { emit(result,71,{input,11,5292});emit(result,71,{input,14});annotations=true; }
    if (!declarations && op==54) { result.insert(result.end(),globals.begin(),globals.end());declarations=true; }
    if (op==54) function=code[i+2];
    if (op==56) function=0;
    if (op==15) {
      result.push_back((n+1)<<16|op);result.insert(result.end(),code+i+1,code+i+n);result.push_back(input);
    } else if (op==253 && function==entry) {
      uint32_t size=id(),x=id(),y=id(),x1=id(),y1=id(),x2=id(),y2=id(),coarse=id(),outer=id();
      emit(result,61,{vec2,size,input});
      emit(result,81,{intType,x,size,0});emit(result,81,{intType,y,size,1});
      emit(result,173,{boolType,x1,x,one});emit(result,173,{boolType,y1,y,one});
      emit(result,173,{boolType,x2,x,two});emit(result,173,{boolType,y2,y,two});
      emit(result,166,{boolType,coarse,x1,y1});emit(result,166,{boolType,outer,x2,y2});
      uint32_t r=id(),g=id(),b=id(),middle=id();
      emit(result,169,{fp,r,coarse,oneF,zeroF});
      emit(result,169,{fp,middle,coarse,oneF,zeroF});
      emit(result,169,{fp,g,outer,zeroF,middle});
      emit(result,169,{fp,b,coarse,zeroF,oneF});
      const uint32_t tint[3]={r,g,b};
      for (const auto& output:outputs) {
        uint32_t value=id();emit(result,61,{output.type,value,output.variable});
        std::vector<uint32_t> channels;
        for (uint32_t c=0;c<output.count;++c) {
          uint32_t original=value;
          if (output.count>1) { original=id();emit(result,81,{fp,original,value,c}); }
          if (output.component+c<3) {
            uint32_t oldPart=id(),tintPart=id(),blend=id();
            emit(result,133,{fp,oldPart,original,rest});
            emit(result,133,{fp,tintPart,tint[output.component+c],weight});
            emit(result,129,{fp,blend,oldPart,tintPart});channels.push_back(blend);
          } else channels.push_back(original); // Alpha is unchanged, not recomputed.
        }
        uint32_t blended=channels[0];
        if (output.count>1) {
          blended=id();result.push_back(uint32_t(channels.size()+3)<<16|80);
          result.push_back(output.type);result.push_back(blended);
          result.insert(result.end(),channels.begin(),channels.end());
        }
        emit(result,62,{output.variable,blended});
      }
      result.insert(result.end(),code+i,code+i+n);changed=true;
    } else result.insert(result.end(),code+i,code+i+n);
    i+=n;
  }
  if (!changed || !annotations || !declarations) return {};
  result[3]=next;
  return result;
}
} // namespace ffr
