// Decode one inline-format record using the exact ISO platform library.
#include <vector>
#include <fstream>
#include <iterator>
#include <cstdio>
#include <dlfcn.h>
#include <unistd.h>
int main(int argc,char**argv){
 alarm(5);if(argc!=2)return 2;
 std::ifstream f(argv[1],std::ios::binary);
 std::vector<unsigned char> v((std::istreambuf_iterator<char>(f)),{});
 if(v.empty())return 2;
 void*l=dlopen("/system/lib64/chipset-sdk-sp/libhilog.so",RTLD_NOW|RTLD_LOCAL);
 if(!l){puts(dlerror());return 2;}
 auto fn=(void(*)(std::vector<unsigned char>&,bool,char*,int,bool))dlsym(l,"_ZN4OHOS9HiviewDFX9DecodeMsgERNSt3__h6vectorIhNS1_9allocatorIhEEEEbPcib");
 if(!fn)return 2;
 for(bool priv:{false,true}){char out[8192]={};auto copy=v;fn(copy,priv,out,sizeof(out),false);printf("flag=%d decoded=%s\n",priv,out);}
}
