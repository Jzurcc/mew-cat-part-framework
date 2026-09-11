#ifndef MEW_ITEM_FRAMEWORK_H
#define MEW_ITEM_FRAMEWORK_H

#include <stdint.h>
#include <windows.h>

#define MOD_NAME "MewItemFramework"

#define MIF_MARKER "__MIF__"

#define RVA_PROCESS_ANIMATION_MERGES 0x009BC070U
#define RVA_FIND_SWF_EXPORT          0x009BB6F0U
#define RVA_APPEND_MOVIE_CLIP        0x00A58440U
#define RVA_FIND_SWF_CHARACTER       0x00A4A190U
#define RVA_GON_INDEX_BY_NAME_CONST  0x00942CA0U
#define RVA_GON_INDEX_BY_NAME        0x00942DA0U

#define PROCESS_MERGES_HOOK_STOLEN_BYTES 15
#define FIND_EXPORT_HOOK_STOLEN_BYTES    15
#define APPEND_CLIP_HOOK_STOLEN_BYTES    15
#define GON_INDEX_HOOK_STOLEN_BYTES      15

#define ITEM_TARGET_GROUP_SIZE 5

#define MAX_PATH_LENGTH          520
#define MAX_LINE_LENGTH          1024
#define MAX_PARTS                2048
#define MAX_BATCH_TARGETS        2048
#define MAX_PENDING_NAMED_FIELDS 2048
#define MAX_ID_LENGTH            128
#define MAX_TARGET_LENGTH        64
#define MAX_GON_STRING_LENGTH    (1024U * 1024U)
#define BINDING_WAIT_YIELD_COUNT 4096
#define MAX_APPLICATION_SWFS     4096

#define APPLICATION_SWF_COUNT_OFFSET        0x114
#define APPLICATION_SWF_ARRAY_OFFSET        0x118
#define SWF_CHARACTER_TABLE_OFFSET          0x30
#define SWF_EXPORT_LIST_OFFSET              0x78
#define SWF_EXPORT_NODE_NAME_OFFSET         0x10
#define SWF_EXPORT_NODE_CHARACTER_ID_OFFSET 0x30

#define MOVIE_CLIP_DATA_OFFSET        0x130
#define MOVIE_CLIP_INLINE_DATA_OFFSET 0x60
#define MOVIE_CLIP_FRAME_COUNT_OFFSET 0x00

#define GON_INT_DATA_OFFSET    0x50
#define GON_FLOAT_DATA_OFFSET  0x58
#define GON_STRING_DATA_OFFSET 0x68
#define GON_TYPE_OFFSET        0xA8
#define GON_TYPE_STRING        1
#define GON_TYPE_NUMBER        2

#define MSVC_STRING_SIZE_OFFSET     0x10
#define MSVC_STRING_CAPACITY_OFFSET 0x18
#define MSVC_STRING_SSO_CAPACITY    15

__declspec(dllexport) void __cdecl
MewItemFramework_Init(void);

__declspec(dllexport) int __cdecl
MewItemFramework_ResolvePart(
    const char* id,
    int32_t* resolvedFrame);

#endif
