#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <fstream>
#include <iostream>
#include <map>
#include "core/libraries/network/net_link.h"
namespace Link = Libraries::Net::Link;
int main() {
  Link::Hello legacy;
  legacy.online_id="Kaniel";legacy.build="92829ac4 events 1f protocol 9";
  legacy.secret="s3cr3t";legacy.cookie.fill(0x11);
  auto before=Link::Encode(legacy);
  auto extended=legacy;extended.fragments=true;
  auto after=Link::Encode(extended);
  assert(after.size()==before.size()+11);
  assert(std::equal(before.begin(),before.end(),after.begin()));
  for(size_t n=before.size();n<after.size()-1;++n)assert(after[n]==0);
  assert(after.back()==1);
  Link::Welcome welcome{true,"welcome",true};
  auto decoded=Link::DecodeWelcome(Link::Encode(welcome));
  assert(decoded && decoded->fragments && decoded->accepted);
  welcome.fragments=false;decoded=Link::DecodeWelcome(Link::Encode(welcome));
  assert(decoded && !decoded->fragments);
  std::cout<<"PASS: optional client capability bytes and legacy welcome fallback\n";
}
