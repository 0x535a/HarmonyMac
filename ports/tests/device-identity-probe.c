/* Read-only original identity API diagnostics; never print serial or UDID. */
#include <ctype.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    void *lib=dlopen("/system/lib64/chipset-sdk-sp/libbegetutil.z.so",RTLD_NOW|RTLD_LOCAL);
    int (*get)(const char *,const char *,char *,unsigned int)=lib?dlsym(lib,"GetParameter"):NULL;
    int (*udid)(char *,int)=lib?dlsym(lib,"GetDevUdid"):NULL;
    if(!get || !udid){puts("IDENTITY_API_SYMBOL_MISSING");return 2;}
    char serial[256]={0};
    const char *keys[]={"ohos.boot.sn","const.product.udid","const.product.devUdid",
                        "const.product.manufacturer","const.product.model"};
    for(unsigned i=0;i<sizeof(keys)/sizeof(keys[0]);i++){
        char value[256]={0};
        int ret=get(keys[i],"",value,sizeof(value));
        printf("IDENTITY_PARAMETER key=%s return=%d length=%zu\n",keys[i],ret,strlen(value));
        if(i==0)memcpy(serial,value,sizeof(serial));
    }
    const char *paths[]={"/sys/class/block/vda/serial","/sys/class/block/vda/device/serial"};
    for(unsigned i=0;i<sizeof(paths)/sizeof(paths[0]);i++){
        char value[256]={0};FILE *f=fopen(paths[i],"r");
        if(!f)continue;
        if(fgets(value,sizeof(value),f)){
            value[strcspn(value,"\r\n")]=0;
            printf("IDENTITY_DISK_SERIAL path=%s length=%zu boot_matches=%d\n",paths[i],strlen(value),
                   *serial && !strcmp(serial,value));
        }
        fclose(f);
    }
    char value[256]={0};int ret=udid(value,sizeof(value)),valid=strlen(value)==64;
    for(size_t i=0;value[i];i++)if(!isxdigit((unsigned char)value[i]))valid=0;
    printf("IDENTITY_GETDEVUDID return=%d length=%zu valid_hex64=%d\n",ret,strlen(value),valid);
    return ret==0 && valid?0:2;
}
