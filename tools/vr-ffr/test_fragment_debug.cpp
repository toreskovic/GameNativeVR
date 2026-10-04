#include "../../app/src/main/cpp/vrffr/fragment_debug.h"
#include <cassert>
#include <fstream>
#include <iterator>
int main(int argc,char** argv) {
  assert(argc==3);
  std::ifstream in(argv[1],std::ios::binary);
  std::vector<char> bytes{std::istreambuf_iterator<char>(in),{}};
  assert(bytes.size()%4==0);
  std::vector<uint32_t> code(bytes.size()/4);memcpy(code.data(),bytes.data(),bytes.size());
  auto patched=ffr::fragmentDebug(code.data(),code.size());
  assert(!patched.empty());
  assert(ffr::fragmentDebug(patched.data(),patched.size()).empty());
  assert(ffr::fragmentDebug(code.data(),4).empty());
  std::ofstream out(argv[2],std::ios::binary);
  out.write(reinterpret_cast<char*>(patched.data()),patched.size()*4);
}
