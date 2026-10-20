/* Explicit Linux Vulkan layer for the isolated OptiScaler prototype.
 * Adds supported FSR4 features to copies of VkDeviceCreateInfo/pNext nodes.
 * Application-owned structures and system driver configuration are untouched.
 */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dlfcn.h>
#include <limits.h>
#include <unistd.h>
#include "vk_chain_sizes.h"

/* LD_PRELOAD bootstrap: Vulkan discovers layers through JSON manifests.
 * Keep that metadata here, write a private manifest for this process, then
 * let the ordinary Vulkan loader build its normal layer dispatch chain.
 * All Vulkan hooks are hidden ELF symbols, so preloading this library cannot
 * bypass the loader by interposing its public vkCreateInstance symbol.
 */
#define FSR4_LAYER_NAME "VK_LAYER_GORDOX_native_fsr4"
static char manifest_directory[PATH_MAX];
static char manifest_file[PATH_MAX];
static pid_t manifest_owner;

static int prepend_environment(const char *name,const char *value) {
    const char *old=getenv(name);
    if(!old || !*old)return setenv(name,value,1);
    size_t length=strlen(value)+strlen(old)+2;
    char *combined=malloc(length);
    if(!combined)return -1;
    snprintf(combined,length,"%s:%s",value,old);
    int result=setenv(name,combined,1);
    free(combined);
    return result;
}

static int environment_has_entry(const char *name,const char *entry) {
    const char *list=getenv(name);
    size_t wanted=strlen(entry);
    for(const char *p=list;p && *p;) {
        const char *end=strchr(p,':');
        size_t length=end?(size_t)(end-p):strlen(p);
        if(length==wanted && !strncmp(p,entry,wanted))return 1;
        p=end?end+1:NULL;
    }
    return 0;
}

static void json_string(FILE *file,const char *value) {
    fputc('"',file);
    for(const unsigned char *p=(const unsigned char *)value;*p;++p) {
        if(*p=='"' || *p=='\\'){fputc('\\',file);fputc(*p,file);}
        else if(*p<32)fprintf(file,"\\u%04x",(unsigned)*p);
        else fputc(*p,file);
    }
    fputc('"',file);
}

static void cleanup_manifest(void) {
    /* A child that forks without exec must not remove its parent's file. */
    if(manifest_owner!=getpid())return;
    if(manifest_file[0])unlink(manifest_file);
    if(manifest_directory[0])rmdir(manifest_directory);
    manifest_file[0]=manifest_directory[0]=0;
}

__attribute__((constructor)) static void bootstrap_layer(void) {
    const char *disabled=getenv("FSR4_BOOTSTRAP");
    if(disabled && !strcmp(disabled,"0"))return;

    Dl_info self;
    char library_path[PATH_MAX];
    if(!dladdr((void *)&bootstrap_layer,&self) || !self.dli_fname ||
       !realpath(self.dli_fname,library_path))goto fail;
    snprintf(manifest_directory,sizeof(manifest_directory),"/tmp/native-fsr4-%ld-XXXXXX",(long)getpid());
    if(!mkdtemp(manifest_directory)){manifest_directory[0]=0;goto fail;}
    manifest_owner=getpid();
    int count=snprintf(manifest_file,sizeof(manifest_file),"%s/layer.json",manifest_directory);
    if(count<0 || (size_t)count>=sizeof(manifest_file))goto fail;
    FILE *file=fopen(manifest_file,"wx");
    if(!file)goto fail;
    fputs("{\"file_format_version\":\"1.2.0\",\"layer\":{\"name\":\"" FSR4_LAYER_NAME
          "\",\"type\":\"GLOBAL\",\"library_path\":",file);
    json_string(file,library_path);
    fputs(",\"api_version\":\"1.3.0\",\"implementation_version\":\"3\","
          "\"description\":\"Native FSR4 FP8 features and RG16 view correction\"}}\n",file);
    int failed=ferror(file);
    if(fclose(file) || failed)goto fail;

    /* Retain other explicit-layer search paths and enabled layers. */
    if(prepend_environment("VK_LAYER_PATH",manifest_directory) ||
       (!environment_has_entry("VK_INSTANCE_LAYERS",FSR4_LAYER_NAME) &&
        prepend_environment("VK_INSTANCE_LAYERS",FSR4_LAYER_NAME)) ||
       setenv("radv_cooperative_matrix2_nv","true",1) ||
       setenv("MESA_VK_DEVICE_SELECT","1002:7550!",0))goto fail;
    unsetenv("PROTON_USE_OPTISCALER");
    setenv("WINEDEBUG","-all",0);
    return;
fail:
    fprintf(stderr,"native-fsr4-layer: automatic bootstrap failed; layer may not be active\n");
    cleanup_manifest();
}

__attribute__((destructor)) static void shutdown_layer(void) {
    cleanup_manifest();
}

typedef struct InstanceState {
    void *key;VkInstance instance;PFN_vkGetInstanceProcAddr gipa;struct InstanceState *next;
    int translator;
} InstanceState;
typedef struct DeviceState {
    void *key;VkDevice device;PFN_vkGetDeviceProcAddr gdpa;struct DeviceState *next;
    FILE *frame_log;uint64_t frame,previous_ns;
    struct ImageState *images;
    uint64_t corrected_views;
    int translator;
} DeviceState;
typedef struct ImageState {
    VkImage image;VkFormat format;VkExtent3D extent;VkImageCreateFlags flags;
    struct ImageState *next;
} ImageState;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static InstanceState *instances;
static DeviceState *devices;
static void *dispatch_key(const void *handle){return handle?*(void *const *)handle:NULL;}
static InstanceState *instance_for(const void *handle) {
    void *key=dispatch_key(handle);pthread_mutex_lock(&lock);InstanceState *s=instances;
    while(s && s->key!=key)s=s->next;pthread_mutex_unlock(&lock);return s;
}
static DeviceState *device_for(VkDevice device) {
    void *key=dispatch_key(device);pthread_mutex_lock(&lock);DeviceState *s=devices;
    while(s && s->key!=key)s=s->next;pthread_mutex_unlock(&lock);return s;
}
static VkBaseOutStructure *find_node(VkBaseOutStructure *p,VkStructureType type){for(;p;p=p->pNext)if(p->sType==type)return p;return NULL;}
static void free_chain(VkBaseOutStructure *p){while(p){VkBaseOutStructure *next=p->pNext;free(p);p=next;}}
static VkBaseOutStructure *copy_chain(const void *p,int *valid) {
    VkBaseOutStructure *head=NULL,**tail=&head;
    for(const VkBaseInStructure *it=p;it;it=it->pNext) {
        size_t size=chain_node_size(it->sType);
        if(!size){fprintf(stderr,"native-fsr4-layer: unsupported pNext tag %d; refusing to alter it\n",it->sType);*valid=0;break;}
        VkBaseOutStructure *node=malloc(size);if(!node){*valid=0;break;}
        memcpy(node,it,size);node->pNext=NULL;*tail=node;tail=&node->pNext;
    }
    if(!*valid){free_chain(head);return NULL;}return head;
}
static void *ensure_node(VkBaseOutStructure **chain,VkStructureType type,size_t size) {
    VkBaseOutStructure *node=find_node(*chain,type);if(node)return node;
    node=calloc(1,size);if(!node)return NULL;node->sType=type;node->pNext=*chain;*chain=node;return node;
}
static int has_extension(const VkExtensionProperties *list,uint32_t count,const char *name){for(uint32_t i=0;i<count;++i)if(!strcmp(list[i].extensionName,name))return 1;return 0;}
static VkResult create_augmented_device(InstanceState *state,VkPhysicalDevice physical,const VkDeviceCreateInfo *input,const VkAllocationCallbacks *allocator,VkDevice *device,PFN_vkCreateDevice next_create) {
    if(state->translator){fprintf(stderr,"native-fsr4-layer: passing through translator device without feature changes\n");return next_create(physical,input,allocator,device);}
    PFN_vkGetPhysicalDeviceProperties properties=(PFN_vkGetPhysicalDeviceProperties)state->gipa(state->instance,"vkGetPhysicalDeviceProperties");
    PFN_vkGetPhysicalDeviceFeatures2 get_features=(PFN_vkGetPhysicalDeviceFeatures2)state->gipa(state->instance,"vkGetPhysicalDeviceFeatures2");
    PFN_vkEnumerateDeviceExtensionProperties enumerate=(PFN_vkEnumerateDeviceExtensionProperties)state->gipa(state->instance,"vkEnumerateDeviceExtensionProperties");
    VkPhysicalDeviceProperties props;properties(physical,&props);
    if(props.vendorID!=0x1002 || props.deviceID!=0x7550)return next_create(physical,input,allocator,device);
    const char *required[]={"VK_KHR_cooperative_matrix","VK_EXT_shader_float8","VK_NV_cooperative_matrix2",
        "VK_KHR_shader_float16_int8","VK_KHR_16bit_storage","VK_KHR_variable_pointers","VK_KHR_vulkan_memory_model","VK_EXT_subgroup_size_control"};
    uint32_t count=0;VkResult result=enumerate(physical,NULL,&count,NULL);if(result)return result;
    VkExtensionProperties *available=calloc(count,sizeof(*available));if(!available)return VK_ERROR_OUT_OF_HOST_MEMORY;
    result=enumerate(physical,NULL,&count,available);if(result){free(available);return result;}
    for(uint32_t i=0;i<sizeof(required)/sizeof(required[0]);++i)if(!has_extension(available,count,required[i])){
        fprintf(stderr,"native-fsr4-layer: required extension unavailable: %s\n",required[i]);free(available);return VK_ERROR_EXTENSION_NOT_PRESENT;
    }
    free(available);
    VkPhysicalDeviceCooperativeMatrix2FeaturesNV cm2={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_2_FEATURES_NV};
    VkPhysicalDeviceShaderFloat8FeaturesEXT f8={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT,.pNext=&cm2};
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR cm={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR,.pNext=&f8};
    VkPhysicalDeviceVulkan13Features f13={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,.pNext=&cm};
    VkPhysicalDeviceVulkan12Features f12={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,.pNext=&f13};
    VkPhysicalDeviceVulkan11Features f11={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,.pNext=&f12};
    VkPhysicalDeviceFeatures2 supported={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,.pNext=&f11};
    get_features(physical,&supported);
    if(!cm.cooperativeMatrix || !cm2.cooperativeMatrixConversions || !f8.shaderFloat8CooperativeMatrix || !f8.shaderFloat8 ||
       !f12.shaderFloat16 || !f11.storageBuffer16BitAccess || !f11.variablePointers || !f12.vulkanMemoryModel ||
       !f13.subgroupSizeControl || !supported.features.shaderInt16 || !supported.features.shaderStorageImageWriteWithoutFormat)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    int valid=1;VkBaseOutStructure *chain=copy_chain(input->pNext,&valid);
    if(!valid)return VK_ERROR_INITIALIZATION_FAILED;
    VkDeviceCreateInfo info=*input;VkPhysicalDeviceFeatures core=input->pEnabledFeatures?*input->pEnabledFeatures:(VkPhysicalDeviceFeatures){0};
    VkPhysicalDeviceFeatures2 *core2=(void *)find_node(chain,VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);
    VkPhysicalDeviceFeatures *features=core2?&core2->features:&core;
    features->shaderInt16=VK_TRUE;features->shaderStorageImageWriteWithoutFormat=VK_TRUE;
    features->shaderStorageImageReadWithoutFormat=supported.features.shaderStorageImageReadWithoutFormat;
    if(!core2)info.pEnabledFeatures=&core;
    VkPhysicalDeviceVulkan11Features *v11=(void *)find_node(chain,VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES);
    VkPhysicalDeviceVulkan12Features *v12=(void *)find_node(chain,VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);
    VkPhysicalDeviceVulkan13Features *v13=(void *)find_node(chain,VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES);
#define ENSURE(type,tag,name) type *name=ensure_node(&chain,tag,sizeof(type));if(!name){result=VK_ERROR_OUT_OF_HOST_MEMORY;goto cleanup;}
    if(v11){v11->storageBuffer16BitAccess=1;v11->variablePointers=1;v11->variablePointersStorageBuffer=1;}
    else {
        ENSURE(VkPhysicalDevice16BitStorageFeatures,VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES,s16)
        s16->storageBuffer16BitAccess=1;
        ENSURE(VkPhysicalDeviceVariablePointersFeatures,VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES,vp)
        vp->variablePointers=1;vp->variablePointersStorageBuffer=1;
    }
    if(v12){v12->shaderFloat16=1;v12->shaderInt8=f12.shaderInt8;v12->vulkanMemoryModel=1;}
    else {
        ENSURE(VkPhysicalDeviceShaderFloat16Int8Features,VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES,fi)
        fi->shaderFloat16=1;fi->shaderInt8=f12.shaderInt8;
        ENSURE(VkPhysicalDeviceVulkanMemoryModelFeatures,VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES,mm)
        mm->vulkanMemoryModel=1;
    }
    if(v13){v13->subgroupSizeControl=1;v13->computeFullSubgroups=f13.computeFullSubgroups;}
    else {
        ENSURE(VkPhysicalDeviceSubgroupSizeControlFeatures,VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES,sg)
        sg->subgroupSizeControl=1;sg->computeFullSubgroups=f13.computeFullSubgroups;
    }
    ENSURE(VkPhysicalDeviceCooperativeMatrixFeaturesKHR,VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR,enable_cm)
    enable_cm->cooperativeMatrix=1;
    ENSURE(VkPhysicalDeviceShaderFloat8FeaturesEXT,VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT,enable_f8)
    enable_f8->shaderFloat8=1;enable_f8->shaderFloat8CooperativeMatrix=1;
    ENSURE(VkPhysicalDeviceCooperativeMatrix2FeaturesNV,VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_2_FEATURES_NV,enable_cm2)
    enable_cm2->cooperativeMatrixConversions=1;
#undef ENSURE
    uint32_t extension_count=input->enabledExtensionCount;
    const char **extension_names=calloc(extension_count+sizeof(required)/sizeof(required[0]),sizeof(*extension_names));
    if(!extension_names){result=VK_ERROR_OUT_OF_HOST_MEMORY;goto cleanup;}
    for(uint32_t i=0;i<extension_count;++i)extension_names[i]=input->ppEnabledExtensionNames[i];
    for(uint32_t i=0;i<sizeof(required)/sizeof(required[0]);++i){int found=0;for(uint32_t j=0;j<extension_count;++j)if(!strcmp(extension_names[j],required[i]))found=1;if(!found)extension_names[extension_count++]=required[i];}
    info.pNext=chain;info.enabledExtensionCount=extension_count;info.ppEnabledExtensionNames=extension_names;
    fprintf(stderr,"native-fsr4-layer: enabling FP8 and cooperative matrix conversions on RX 9070 XT\n");
    result=next_create(physical,&info,allocator,device);free(extension_names);
cleanup:
    free_chain(chain);return result;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance,const char *);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice,const char *);
VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo *info,const VkAllocationCallbacks *alloc,VkInstance *instance) {
    VkLayerInstanceCreateInfo *link=(void *)info->pNext;
    while(link && !(link->sType==VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && link->function==VK_LAYER_LINK_INFO))link=(void *)link->pNext;
    if(!link)return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr next_gipa=link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkCreateInstance next_create=(PFN_vkCreateInstance)next_gipa(NULL,"vkCreateInstance");
    link->u.pLayerInfo=link->u.pLayerInfo->pNext;
    InstanceState *state=calloc(1,sizeof(*state));if(!state)return VK_ERROR_OUT_OF_HOST_MEMORY;
    VkResult result=next_create(info,alloc,instance);if(result){free(state);return result;}
    state->key=dispatch_key(*instance);state->instance=*instance;state->gipa=next_gipa;
    const char *engine=info->pApplicationInfo?info->pApplicationInfo->pEngineName:NULL;
    state->translator=engine && (!strcmp(engine,"vkd3d") || !strcmp(engine,"DXVK"));
    pthread_mutex_lock(&lock);state->next=instances;instances=state;pthread_mutex_unlock(&lock);return result;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyInstance(VkInstance instance,const VkAllocationCallbacks *alloc) {
    InstanceState *state=instance_for(instance);if(!state)return;
    PFN_vkDestroyInstance destroy=(PFN_vkDestroyInstance)state->gipa(instance,"vkDestroyInstance");
    destroy(instance,alloc);pthread_mutex_lock(&lock);InstanceState **p=&instances;while(*p && *p!=state)p=&(*p)->next;if(*p)*p=state->next;pthread_mutex_unlock(&lock);free(state);
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(VkPhysicalDevice physical,const VkDeviceCreateInfo *info,const VkAllocationCallbacks *alloc,VkDevice *device) {
    InstanceState *parent=instance_for(physical);if(!parent)return VK_ERROR_INITIALIZATION_FAILED;
    VkLayerDeviceCreateInfo *link=(void *)info->pNext;
    while(link && !(link->sType==VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && link->function==VK_LAYER_LINK_INFO))link=(void *)link->pNext;
    if(!link)return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr gipa=link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr gdpa=link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    PFN_vkCreateDevice next_create=(PFN_vkCreateDevice)gipa(parent->instance,"vkCreateDevice");
    link->u.pLayerInfo=link->u.pLayerInfo->pNext;
    DeviceState *state=calloc(1,sizeof(*state));if(!state)return VK_ERROR_OUT_OF_HOST_MEMORY;
    VkResult result=create_augmented_device(parent,physical,info,alloc,device,next_create);
    if(result){free(state);return result;}
    state->key=dispatch_key(*device);state->device=*device;state->gdpa=gdpa;state->translator=parent->translator;pthread_mutex_lock(&lock);state->next=devices;devices=state;pthread_mutex_unlock(&lock);return result;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyDevice(VkDevice device,const VkAllocationCallbacks *alloc) {
    DeviceState *state=device_for(device);if(!state)return;
    PFN_vkDestroyDevice destroy=(PFN_vkDestroyDevice)state->gdpa(device,"vkDestroyDevice");
    destroy(device,alloc);pthread_mutex_lock(&lock);DeviceState **p=&devices;while(*p && *p!=state)p=&(*p)->next;if(*p)*p=state->next;pthread_mutex_unlock(&lock);if(state->frame_log)fclose(state->frame_log);
    while(state->images){ImageState *image=state->images;state->images=image->next;free(image);}free(state);
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateImage(VkDevice device,const VkImageCreateInfo *info,const VkAllocationCallbacks *alloc,VkImage *image) {
    DeviceState *state=device_for(device);if(!state)return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkCreateImage next=(PFN_vkCreateImage)state->gdpa(device,"vkCreateImage");
    if(state->translator)return next(device,info,alloc,image);
    VkResult result=next(device,info,alloc,image);
    if(result==VK_SUCCESS) {
        ImageState *record=calloc(1,sizeof(*record));
        if(record){record->image=*image;record->format=info->format;record->extent=info->extent;record->flags=info->flags;
            pthread_mutex_lock(&lock);record->next=state->images;state->images=record;pthread_mutex_unlock(&lock);}
        if(info->format==VK_FORMAT_R16G16_SNORM || info->format==VK_FORMAT_R16G16_UNORM)
            fprintf(stderr,"native-fsr4-layer: normalized RG16 image=%p format=%u extent=%ux%u flags=0x%x usage=0x%x\n",(void *)*image,info->format,info->extent.width,info->extent.height,info->flags,info->usage);
    }
    return result;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyImage(VkDevice device,VkImage image,const VkAllocationCallbacks *alloc) {
    DeviceState *state=device_for(device);if(!state)return;
    pthread_mutex_lock(&lock);ImageState **p=&state->images;while(*p && (*p)->image!=image)p=&(*p)->next;
    if(*p){ImageState *record=*p;*p=record->next;free(record);}pthread_mutex_unlock(&lock);
    PFN_vkDestroyImage next=(PFN_vkDestroyImage)state->gdpa(device,"vkDestroyImage");next(device,image,alloc);
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateImageView(VkDevice device,const VkImageViewCreateInfo *info,const VkAllocationCallbacks *alloc,VkImageView *view) {
    DeviceState *state=device_for(device);if(!state)return VK_ERROR_INITIALIZATION_FAILED;
    if(state->translator){PFN_vkCreateImageView next=(PFN_vkCreateImageView)state->gdpa(device,"vkCreateImageView");return next(device,info,alloc,view);}
    VkImageViewCreateInfo local=*info;VkFormat actual=VK_FORMAT_UNDEFINED;VkExtent3D extent={0};
    pthread_mutex_lock(&lock);for(ImageState *it=state->images;it;it=it->next)if(it->image==info->image){actual=it->format;extent=it->extent;break;}pthread_mutex_unlock(&lock);
    /* OptiScaler 0.9.4 maps these two RG16 formats to RGBA8 in its FFX
     * resource descriptor. Preserve the original normalized texels when the
     * provider makes the corresponding sampled view; do not reinterpret bits. */
    if((actual==VK_FORMAT_R16G16_SNORM && info->format==VK_FORMAT_R8G8B8A8_SNORM) ||
       (actual==VK_FORMAT_R16G16_UNORM && info->format==VK_FORMAT_R8G8B8A8_UNORM)) {
        const char *disable=getenv("FSR4_DISABLE_RG16_VIEW_FIX");
        if(!disable || strcmp(disable,"1"))local.format=actual;
        pthread_mutex_lock(&lock);uint64_t correction=++state->corrected_views;pthread_mutex_unlock(&lock);
        if(correction<=16 || correction%600==0)
            fprintf(stderr,"native-fsr4-layer: RG16 view image=%p requested=%u actual=%u used=%u extent=%ux%u count=%llu\n",(void *)info->image,info->format,actual,local.format,extent.width,extent.height,(unsigned long long)correction);
    }
    PFN_vkCreateImageView next=(PFN_vkCreateImageView)state->gdpa(device,"vkCreateImageView");return next(device,&local,alloc,view);
}
VKAPI_ATTR VkResult VKAPI_CALL vkQueuePresentKHR(VkQueue queue,const VkPresentInfoKHR *info) {
    DeviceState *state=device_for((VkDevice)queue);if(!state)return VK_ERROR_INITIALIZATION_FAILED;
    struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);
    uint64_t ns=(uint64_t)now.tv_sec*1000000000ull+(uint64_t)now.tv_nsec;
    if(!state->frame_log){
        state->frame_log=fopen("native-fsr4-frame-times.csv","a");
        if(state->frame_log && ftell(state->frame_log)==0)fprintf(state->frame_log,"frame,monotonic_ns,present_interval_ns\n");
    }
    if(state->frame_log){fprintf(state->frame_log,"%llu,%llu,%llu\n",(unsigned long long)state->frame,(unsigned long long)ns,(unsigned long long)(state->previous_ns?ns-state->previous_ns:0));if(state->frame%120==0)fflush(state->frame_log);}
    state->previous_ns=ns;++state->frame;
    PFN_vkQueuePresentKHR next=(PFN_vkQueuePresentKHR)state->gdpa(state->device,"vkQueuePresentKHR");
    return next(queue,info);
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance,const char *name) {
    if(!strcmp(name,"vkGetInstanceProcAddr"))return (PFN_vkVoidFunction)vkGetInstanceProcAddr;
    if(!strcmp(name,"vkGetDeviceProcAddr"))return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
    if(!strcmp(name,"vkCreateInstance"))return (PFN_vkVoidFunction)vkCreateInstance;
    if(!strcmp(name,"vkDestroyInstance"))return (PFN_vkVoidFunction)vkDestroyInstance;
    if(!strcmp(name,"vkCreateDevice"))return (PFN_vkVoidFunction)vkCreateDevice;
    if(!strcmp(name,"vkQueuePresentKHR"))return (PFN_vkVoidFunction)vkQueuePresentKHR;
    if(!strcmp(name,"vkCreateImage"))return (PFN_vkVoidFunction)vkCreateImage;
    if(!strcmp(name,"vkDestroyImage"))return (PFN_vkVoidFunction)vkDestroyImage;
    if(!strcmp(name,"vkCreateImageView"))return (PFN_vkVoidFunction)vkCreateImageView;
    InstanceState *state=instance_for(instance);return state?state->gipa(instance,name):NULL;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device,const char *name) {
    if(!strcmp(name,"vkGetDeviceProcAddr"))return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
    if(!strcmp(name,"vkDestroyDevice"))return (PFN_vkVoidFunction)vkDestroyDevice;
    if(!strcmp(name,"vkQueuePresentKHR"))return (PFN_vkVoidFunction)vkQueuePresentKHR;
    if(!strcmp(name,"vkCreateImage"))return (PFN_vkVoidFunction)vkCreateImage;
    if(!strcmp(name,"vkDestroyImage"))return (PFN_vkVoidFunction)vkDestroyImage;
    if(!strcmp(name,"vkCreateImageView"))return (PFN_vkVoidFunction)vkCreateImageView;
    DeviceState *state=device_for(device);return state?state->gdpa(device,name):NULL;
}
__attribute__((visibility("default"))) VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *version) {
    if(version->loaderLayerInterfaceVersion>2)version->loaderLayerInterfaceVersion=2;
    version->pfnGetInstanceProcAddr=vkGetInstanceProcAddr;version->pfnGetDeviceProcAddr=vkGetDeviceProcAddr;
    version->pfnGetPhysicalDeviceProcAddr=NULL;return VK_SUCCESS;
}
