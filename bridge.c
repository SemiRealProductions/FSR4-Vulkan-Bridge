/* Experimental legacy FFX Vulkan DLL interface for OptiScaler 0.9.4.
 * Public FFX signatures/resources are unchanged. The driver provider and copied
 * signed companion DLLs live beside this bridge in its separate testing folder.
 */
/* ---- AMD provider interface ---- */
#ifndef INVESTIGATION_PROVIDER_ABI_H
#define INVESTIGATION_PROVIDER_ABI_H
#include <stdint.h>
#include <stddef.h>

/* Recovered for amdvkffx64.dll SHA256 80e761f7...bc58bd, Windows x64 only.
 * Evidence: logs/provider_exports.asm and logs/driver_loader.asm.
 * Callback signatures also match the public SDK external-provider header.
 * Vulkan backend tag translation and dispatch layout are documented in REPORT.md.
 */
typedef struct ProviderV2 {
    uint32_t version;                    /* +00: 2 */
    uint32_t padding;
    uint64_t descriptor_type;            /* +08: 0x10000 = upscale create */
    uint64_t implementation_id;          /* +10: output */
    const char *implementation_name;     /* +18: output */
    void *can_provide;                   /* +20: output */
    void *create_context;                /* +28: output */
    void *destroy_context;               /* +30: output */
    void *configure;                     /* +38: output */
    void *query;                         /* +40: output */
    void *dispatch;                      /* +48: output */
} ProviderV2;

typedef struct ProviderSetting {
    uint32_t value_type;                 /* +00: 0 = byte boolean */
    uint32_t setting_id;                 /* +04: 0 = enable upscaler */
    uint8_t enabled;                     /* +08 */
    uint8_t padding[7];
    struct ProviderSetting *next;        /* +10 */
} ProviderSetting;

typedef int32_t (*UpdateProvider)(ProviderV2 *, uint32_t);
typedef int32_t (*UpdateProviderEx)(ProviderV2 *, uint32_t, const ProviderSetting *);

_Static_assert(sizeof(void *) == 8, "Windows x64 required");
_Static_assert(sizeof(ProviderV2) == 0x50, "table size");
_Static_assert(offsetof(ProviderV2, descriptor_type) == 8, "descriptor offset");
_Static_assert(offsetof(ProviderV2, dispatch) == 0x48, "dispatch offset");
_Static_assert(sizeof(ProviderSetting) == 0x18, "setting size");
_Static_assert(offsetof(ProviderSetting, next) == 0x10, "next offset");
#endif
/* ---- End AMD provider interface ---- */
#include <vulkan/vulkan_core.h>
__declspec(dllimport) int GetModuleHandleExA(uint32_t,const char *,void **);
__declspec(dllimport) uint32_t GetModuleFileNameA(void *,char *,uint32_t);
__declspec(dllimport) void *LoadLibraryExA(const char *,void *,uint32_t);
__declspec(dllimport) void *GetProcAddress(void *,const char *);
__declspec(dllimport) void *CreateFileA(const char *,uint32_t,uint32_t,void *,uint32_t,uint32_t,void *);
__declspec(dllimport) int WriteFile(void *,const void *,uint32_t,uint32_t *,void *);
__declspec(dllimport) void OutputDebugStringA(const char *);
__declspec(dllimport) void Sleep(uint32_t);
void *memcpy(void *dst,const void *src,size_t n){unsigned char *d=dst;const unsigned char *s=src;for(size_t i=0;i<n;++i)d[i]=s[i];return dst;}
void *memset(void *dst,int value,size_t n){volatile unsigned char *d=dst;for(size_t i=0;i<n;++i)d[i]=(unsigned char)value;return dst;}
static void *log_handle;
static void out(const char *text){uint32_t n=0,written=0;while(text[n])++n;if(log_handle && log_handle!=(void *)(intptr_t)-1)WriteFile(log_handle,text,n,&written,0);}
static void field(const char *name,uint64_t value){char s[19];const char *digits="0123456789abcdef";s[0]='0';s[1]='x';s[18]=0;for(int i=17;i>=2;--i){s[i]=digits[value&15];value>>=4;}out(name);out(s);out("\r\n");}
static int eq(const char *a,const char *b){while(*a && *a==*b){++a;++b;}return *a==*b;}
/* ---- SPIR-V image compatibility adapter ---- */
/* Experimental image-interface adapter, equivalent to tools/widen_image_types.py.
 * No FP8/matrix instructions, weights, resource formats or shader bindings change.
 * This preserves all original result IDs by inserting FConvert at image boundaries.
 */
__declspec(dllimport) void *GetProcessHeap(void);
__declspec(dllimport) void *HeapAlloc(void *,uint32_t,size_t);
__declspec(dllimport) int HeapFree(void *,uint32_t,void *);
typedef struct AdapterType {uint32_t opcode,component,count;} AdapterType;
static uint32_t *adapt_image_types(const uint32_t *input,size_t bytes,size_t *out_bytes) {
    if(bytes<20 || bytes%4 || input[0]!=0x07230203 || input[3]>2000000) return 0;
    const uint32_t limit=input[3],words=(uint32_t)(bytes/4);
    void *heap=GetProcessHeap();
    AdapterType *types=HeapAlloc(heap,8,(size_t)limit*sizeof(*types));
    uint32_t *wide=HeapAlloc(heap,8,(size_t)limit*4);
    uint32_t *ids=HeapAlloc(heap,8,(size_t)limit*4);
    uint32_t *extra=HeapAlloc(heap,0,(size_t)limit*16);
    uint32_t *output=HeapAlloc(heap,0,bytes*3+(size_t)limit*16+256);
    int valid=types && wide && ids && extra && output;
    uint32_t bound=limit,extra_count=0,f32=0,images=0,reads=0,writes=0;
    if(!valid)goto cleanup;
    for(uint32_t i=5;i<words;) {
        uint32_t n=input[i]>>16,op=input[i]&65535;
        if(!n || i+n>words){valid=0;goto cleanup;}
        const uint32_t *a=input+i+1;
        if((op>=19 && op<=39) || op==4456) {
            if(n<2 || a[0]>=limit){valid=0;goto cleanup;}
            types[a[0]].opcode=op;
            if(n>=3)types[a[0]].component=a[1];
            if(n>=4)types[a[0]].count=a[2];
            if(op==22 && n==3 && a[1]==32)f32=a[0];
        }
        /* Sparse image result structures need a separate transformation. */
        if(op>=305 && op<=316){valid=0;goto cleanup;}
        i+=n;
    }
    uint32_t half_images=0;
    for(uint32_t i=1;i<limit;++i)
        if(types[i].opcode==25 && types[i].component<limit && types[types[i].component].opcode==22 && types[types[i].component].component==16)++half_images;
    if(!half_images){memcpy(output,input,bytes);*out_bytes=bytes;out("SPIR-V image adapter passthrough\r\n");goto cleanup;}
    if(!f32){valid=0;goto cleanup;}
    for(uint32_t i=1;i<limit;++i)
        if(types[i].opcode==22 && types[i].component==16)wide[i]=f32;
    for(uint32_t i=1;i<limit;++i) {
        if(types[i].opcode!=23 || types[i].component>=limit || !wide[types[i].component])continue;
        for(uint32_t j=1;j<limit;++j)
            if(types[j].opcode==23 && types[j].component==f32 && types[j].count==types[i].count){wide[i]=j;break;}
        if(!wide[i]) {
            wide[i]=bound++;
            extra[extra_count++]=(4u<<16)|23;extra[extra_count++]=wide[i];
            extra[extra_count++]=f32;extra[extra_count++]=types[i].count;
        }
    }
    for(uint32_t i=5;i<words;) {
        uint32_t n=input[i]>>16,op=input[i]&65535;
        const uint32_t *a=input+i+1;
        if(n>=3 && a[0]<limit && types[a[0]].opcode && a[1]<limit && !((op>=19 && op<=39)||op==4456))ids[a[1]]=a[0];
        i+=n;
    }
    uint32_t at=5;memcpy(output,input,20);int inserted=0;
    for(uint32_t i=5;i<words;) {
        uint32_t n=input[i]>>16,op=input[i]&65535;const uint32_t *a=input+i+1;
        if(op==54 && !inserted){memcpy(output+at,extra,(size_t)extra_count*4);at+=extra_count;inserted=1;}
        if(op==99 && n>=4 && a[2]<limit && ids[a[2]]<limit && wide[ids[a[2]]]) {
            uint32_t temp=bound++;
            output[at++]=(4u<<16)|115;output[at++]=wide[ids[a[2]]];output[at++]=temp;output[at++]=a[2];
            memcpy(output+at,input+i,(size_t)n*4);output[at+3]=temp;at+=n;++writes;
        } else if(op>=87 && op<=98 && n>=4 && a[0]<limit && wide[a[0]]) {
            uint32_t temp=bound++;
            memcpy(output+at,input+i,(size_t)n*4);output[at+1]=wide[a[0]];output[at+2]=temp;at+=n;
            output[at++]=(4u<<16)|115;output[at++]=a[0];output[at++]=a[1];output[at++]=temp;++reads;
        } else {
            memcpy(output+at,input+i,(size_t)n*4);
            if(op==25 && n>=3 && a[1]<limit && types[a[1]].opcode==22 && types[a[1]].component==16){output[at+2]=f32;++images;}
            at+=n;
        }
        i+=n;
    }
    if(!inserted){valid=0;goto cleanup;}
    output[3]=bound;*out_bytes=(size_t)at*4;
    field("SPIR-V widened image types=",images);field("SPIR-V image read conversions=",reads);field("SPIR-V image write conversions=",writes);
cleanup:
    if(types)HeapFree(heap,0,types);if(wide)HeapFree(heap,0,wide);if(ids)HeapFree(heap,0,ids);if(extra)HeapFree(heap,0,extra);
    if(!valid && output){HeapFree(heap,0,output);output=0;}
    return output;
}
/* ---- End SPIR-V image compatibility adapter ---- */
typedef struct ApiHeader {uint64_t type;struct ApiHeader *next;} ApiHeader;
/* ---- FFX dispatch and resource interface ---- */
#ifndef NATIVE_FSR4_DISPATCH_ABI_H
#define NATIVE_FSR4_DISPATCH_ABI_H
/* Public FFX 1.1.4 resource/dispatch layout, checked against the 4.1.2
 * provider's dispatch loads. ApiHeader must be defined by the including file. */
typedef struct ResourceDescription {
    uint32_t type,format,width,height,depth,mip_count,flags,usage;
} ResourceDescription;
typedef struct Resource {
    void *resource; ResourceDescription description; uint32_t state;
} Resource;
typedef struct DispatchDesc {
    ApiHeader header; void *command_list;
    Resource color,depth,motion,exposure,reactive,transparency,output;
    float jitter[2],motion_scale[2];
    uint32_t render_size[2],upscale_size[2];
    uint8_t sharpen; uint8_t padding0[3];
    float sharpness,frame_time_ms,pre_exposure;
    uint8_t reset; uint8_t padding1[3];
    float camera_near,camera_far,camera_fov,view_to_meters;
    uint32_t flags;
} DispatchDesc;
_Static_assert(sizeof(Resource)==0x30,"resource ABI");
_Static_assert(offsetof(DispatchDesc,output)==0x138,"output ABI");
_Static_assert(offsetof(DispatchDesc,pre_exposure)==0x194,"exposure ABI");
_Static_assert(sizeof(DispatchDesc)==0x1b0,"dispatch ABI");
#endif
/* ---- End FFX dispatch and resource interface ---- */
typedef struct BackendVKDesc {ApiHeader header;VkDevice device;VkPhysicalDevice physical;PFN_vkGetDeviceProcAddr get_device_proc;} BackendVKDesc;
typedef struct UpscaleDesc {ApiHeader header;uint32_t flags,render_width,render_height,output_width,output_height;void *message;} UpscaleDesc;
typedef uint32_t (*CreateContext)(void **,const void *,const void *);
typedef uint32_t (*DestroyContext)(void **,const void *);
typedef uint32_t (*Operation)(void **,const void *);
static ProviderV2 provider;
static void *provider_module;
static PFN_vkGetDeviceProcAddr original_gdpa;
static int init_state;
static char directory[4096];
static uint32_t directory_size;
static uint64_t dispatch_count;
__declspec(dllexport) uint32_t ffxCreateContext(void **,ApiHeader *,const void *);
static void *load_local(const char *name){char path[4096];uint32_t i=0;memcpy(path,directory,directory_size);while(name[i] && directory_size+i<4095){path[directory_size+i]=name[i];++i;}path[directory_size+i]=0;return LoadLibraryExA(path,0,0);}
static int initialize(void) {
    int expected=0;
    if(__atomic_compare_exchange_n(&init_state,&expected,1,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) {
        void *self=0;
        if(!GetModuleHandleExA(5,(const char *)&ffxCreateContext,&self))goto fail;
        uint32_t n=GetModuleFileNameA(self,directory,sizeof(directory));
        if(!n || n>=sizeof(directory))goto fail;
        for(uint32_t i=0;i<n;++i)if(directory[i]=='\\' || directory[i]=='/')directory_size=i+1;
        if(!directory_size)goto fail;
        char path[4096];const char *name="native-fsr4-bridge.log";uint32_t i=0;
        memcpy(path,directory,directory_size);while(name[i]){path[directory_size+i]=name[i];++i;}path[directory_size+i]=0;
        log_handle=CreateFileA(path,4,3,0,4,0x80,0);
        out("native-fsr4-bridge: initializing experimental FSR 4.1.2 FP8 Vulkan provider\r\n");
        if(!load_local("amd_fidelityfx_loader_vk.dll") || !load_local("amd_fidelityfx_upscaler_vk.dll"))goto fail;
        provider_module=load_local("amdvkffx64.dll");if(!provider_module)goto fail;
        UpdateProviderEx update=(UpdateProviderEx)GetProcAddress(provider_module,"UpdateFfxApiProviderEx");if(!update)goto fail;
        provider.version=2;provider.descriptor_type=0x10000;
        ProviderSetting setting={0};setting.enabled=1;
        int32_t result=update(&provider,sizeof(provider),&setting);field("Provider registration HRESULT=",(uint32_t)result);
        if(result || provider.implementation_id!=0xf5a5ca1e81001002ull)goto fail;
        __atomic_store_n(&init_state,2,__ATOMIC_RELEASE);return 1;
fail:
        out("native-fsr4-bridge: initialization failed\r\n");__atomic_store_n(&init_state,-1,__ATOMIC_RELEASE);return 0;
    }
    while(__atomic_load_n(&init_state,__ATOMIC_ACQUIRE)==1)Sleep(1);
    return __atomic_load_n(&init_state,__ATOMIC_ACQUIRE)==2;
}
static VkResult VKAPI_CALL create_shader(VkDevice device,const VkShaderModuleCreateInfo *info,const VkAllocationCallbacks *allocator,VkShaderModule *module) {
    size_t size=0;uint32_t *code=adapt_image_types(info->pCode,info->codeSize,&size);
    if(!code){out("native-fsr4-bridge: unsupported shader image interface\r\n");return VK_ERROR_INITIALIZATION_FAILED;}
    VkShaderModuleCreateInfo local=*info;local.pCode=code;local.codeSize=size;
    PFN_vkCreateShaderModule real=(PFN_vkCreateShaderModule)original_gdpa(device,"vkCreateShaderModule");
    VkResult result=real(device,&local,allocator,module);HeapFree(GetProcessHeap(),0,code);return result;
}
static VkResult VKAPI_CALL create_compute(VkDevice device,VkPipelineCache cache,uint32_t count,const VkComputePipelineCreateInfo *infos,const VkAllocationCallbacks *allocator,VkPipeline *pipelines) {
    for(uint32_t i=0;i<count;++i){out("native-fsr4-bridge compute: ");out(infos[i].stage.pName);out("\r\n");}
    PFN_vkCreateComputePipelines real=(PFN_vkCreateComputePipelines)original_gdpa(device,"vkCreateComputePipelines");
    return real(device,cache,count,infos,allocator,pipelines);
}
static PFN_vkVoidFunction VKAPI_CALL adapted_gdpa(VkDevice device,const char *name) {
    if(eq(name,"vkCreateShaderModule"))return (PFN_vkVoidFunction)create_shader;
    if(eq(name,"vkCreateComputePipelines"))return (PFN_vkVoidFunction)create_compute;
    return original_gdpa(device,name);
}
__declspec(dllexport) uint32_t ffxCreateContext(void **context,ApiHeader *header,const void *allocator) {
    if(!context || !header || header->type!=0x10000)return 6;
    *context=0;if(!initialize())return 4;
    const BackendVKDesc *backend=0;
    struct {ApiHeader header;uint64_t version;} override={{5,0},0};
    for(const ApiHeader *p=header->next;p;p=p->next) {
        if(p->type==3 || p->type==0x01000003)backend=(const BackendVKDesc *)p;
        else if(p->type==5)override.version=*(const uint64_t *)((const unsigned char *)p+16);
        else if(p->type!=0x1000b){field("Unsupported create extension type=",p->type);return 2;}
    }
    if(!backend || !backend->device || !backend->physical || !backend->get_device_proc)return 6;
    if(override.version && override.version!=provider.implementation_id)return 4;
    original_gdpa=backend->get_device_proc;
    BackendVKDesc backend_copy=*backend;backend_copy.header.type=0x01000003;backend_copy.get_device_proc=adapted_gdpa;
    struct {ApiHeader header;uint32_t version;} version={{0x1000b,override.version?&override.header:0},0x1001001};
    backend_copy.header.next=&version.header;
    UpscaleDesc desc=*(const UpscaleDesc *)header;desc.header.next=&backend_copy.header;
    field("Context flags=",desc.flags);
    uint32_t result=((CreateContext)provider.create_context)(context,&desc,allocator);
    field("Native Vulkan FSR4 CreateContext return=",result);
    if(!result && *context) {
        void *selected=((void **)*context)[1];
        field("Selected provider object RVA=",(uintptr_t)selected-(uintptr_t)provider_module);
        if((uintptr_t)selected-(uintptr_t)provider_module!=0x43cc190) {
            ((DestroyContext)provider.destroy_context)(context,allocator);*context=0;
            out("native-fsr4-bridge: refusing non-FP8 provider fallback\r\n");return 4;
        }
        out("native-fsr4-bridge: FSR 4.1.2 FP8 native Vulkan context active\r\n");
    }
    return result;
}
__declspec(dllexport) uint32_t ffxDestroyContext(void **context,const void *allocator) {
    if(!context || !*context || !initialize())return 6;
    uint32_t result=((DestroyContext)provider.destroy_context)(context,allocator);if(!result)*context=0;return result;
}
__declspec(dllexport) uint32_t ffxConfigure(void **context,const ApiHeader *header) {
    if(!header || !initialize())return 6;
    if(header->type==1 || header->type==7)return 0;
    return ((Operation)provider.configure)(context,header);
}
__declspec(dllexport) uint32_t ffxQuery(void **context,ApiHeader *header) {
    if(!header || !initialize())return 6;
    if(header->type==4) {
        struct Versions {ApiHeader header;uint64_t create_type;void *device;uint64_t *count,*ids;const char **names;};
        struct Versions *v=(void *)header;if(!v->count)return 6;
        uint64_t capacity=*v->count;*v->count=(v->create_type&0xff0000)==0x10000?1:0;
        if(*v->count && capacity){if(v->ids)v->ids[0]=provider.implementation_id;if(v->names)v->names[0]="4.1.2 * native Vulkan FP8";}
        return 0;
    }
    if(header->type==6) {
        struct Version {ApiHeader header;uint64_t id;const char *name;};struct Version *v=(void *)header;
        v->id=context && *context?provider.implementation_id:0;v->name=v->id?"4.1.2 * native Vulkan FP8":0;return 0;
    }
    return ((Operation)provider.query)(context,header);
}
__declspec(dllexport) uint32_t ffxDispatch(void **context,const ApiHeader *header) {
    if(!context || !*context || !header || !initialize())return 6;
    uint64_t count=__atomic_add_fetch(&dispatch_count,1,__ATOMIC_RELAXED);
    DispatchDesc local;
    if(header->type==0x10001) {
        local=*(const DispatchDesc *)header;
        /* Normal operation is fixed at the previously tested controls=0.
         * No diagnostics file polling, motion overrides or periodic dumps. */
        header=&local.header;
        if(count<=3) {
            const Resource *r[]={&local.color,&local.depth,&local.motion,&local.exposure,&local.reactive,&local.transparency,&local.output};
            const char *names[]={"Color","Depth","Motion","Exposure","Reactive","Transparency","Output"};
            for(uint32_t i=0;i<7;++i){out(names[i]);out(" resource\r\n");field(" image=",(uintptr_t)r[i]->resource);field(" format=",r[i]->description.format);field(" width=",r[i]->description.width);field(" height=",r[i]->description.height);field(" usage=",r[i]->description.usage);field(" state=",r[i]->state);}
            const float values[]={local.jitter[0],local.jitter[1],local.motion_scale[0],local.motion_scale[1],local.frame_time_ms,local.pre_exposure,local.camera_near,local.camera_far,local.camera_fov,local.view_to_meters};
            const char *names2[]={"Jitter X bits=","Jitter Y bits=","MV scale X bits=","MV scale Y bits=","Frame ms bits=","Pre-exposure bits=","Camera near bits=","Camera far bits=","Camera FOV bits=","View to meters bits="};
            for(uint32_t i=0;i<10;++i){uint32_t bits;memcpy(&bits,&values[i],4);field(names2[i],bits);}
            field("Reset=",local.reset);field("Dispatch flags=",local.flags);
        }
    }
    uint32_t result=((Operation)provider.dispatch)(context,header);
    if(count==1 || result){field("Native Vulkan FSR4 dispatch count=",count);field("Native Vulkan FSR4 dispatch result=",result);}
    return result;
}
