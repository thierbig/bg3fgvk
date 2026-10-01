// fgvk-tests.exe: host-only unit tests (no Vulkan/Streamline dependency).
#include <cstdio>
int test_mvecscale();
int test_exportroute();
int test_slmodule();
int test_presentpacer();
int main(){
  int fails = test_mvecscale() + test_exportroute() + test_slmodule() + test_presentpacer();
  printf("%d failure(s)\n", fails);
  return fails ? 1 : 0;
}
