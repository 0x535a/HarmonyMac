// Decode selected raw socket records using the exact product HiLog library.
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <unistd.h>

int main(int argc,char **argv)
{
    if(argc!=3)return 2;
    alarm(40);
    std::ifstream input(argv[1],std::ios::binary);
    std::string raw((std::istreambuf_iterator<char>(input)),{});
    if(raw.empty())return 2;
    void *lib=dlopen("/system/lib64/chipset-sdk-sp/libhilog.so",RTLD_NOW|RTLD_LOCAL);
    if(!lib){puts(dlerror());return 2;}
    auto decode=(void(*)(std::vector<unsigned char>&,bool,char*,int,bool))dlsym(lib,
        "_ZN4OHOS9HiviewDFX9DecodeMsgERNSt3__h6vectorIhNS1_9allocatorIhEEEEbPcib");
    if(!decode)return 2;
    const std::string marker="===HILOGRAM===\n";
    size_t cursor=0,count=0;
    while((cursor=raw.find(marker,cursor))!=std::string::npos){
        size_t begin=cursor+marker.size();
        size_t end=raw.find(marker,begin);
        if(end==std::string::npos)end=raw.size();
        cursor=begin;
        if(end-begin<33)continue;
        const unsigned char *record=(const unsigned char*)raw.data()+begin;
        unsigned length=record[0]|(record[1]<<8);
        unsigned bits=record[2]|(record[3]<<8);
        unsigned tag_length=bits>>10;
        size_t payload=32+tag_length;
        if(length>end-begin || payload>=length)continue;
        std::string selected(raw.data()+begin,length);
        if(selected.find(argv[2])==std::string::npos)continue;
        unsigned monotonic=0,tid=0;
        memcpy(&monotonic,record+12,4);memcpy(&tid,record+24,4);
        printf("RECORD mono=%u tid=%u tag=%.*s bytes=%u\n",monotonic,tid,
               (int)tag_length,(const char*)record+32,length);
        std::vector<unsigned char> content(record+payload,record+length);
        char output[16384]={};
        decode(content,false,output,sizeof(output),false);
        printf("%s\n",output);count++;
    }
    printf("DECODED_RECORDS=%zu\n",count);
    return count?0:2;
}
