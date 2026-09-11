#include "MewItemFramework.h"
#include "mewjector.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void* (__fastcall *fn_find_swf_export)(void* application, void* name);
typedef void* (__fastcall *fn_find_swf_character)(void* characterTable, int32_t characterId);
typedef void  (__fastcall *fn_append_movie_clip)(void* destination, void* source);
typedef void* (__fastcall *fn_gon_index_by_name)(void* gonObject, const void* fieldName);
typedef void  (__fastcall *fn_process_animation_merges)(void* application, void* swf);

static const char* const WEAPON_TARGETS[4] = {
    "Weapon", "WeaponIcon", "WeaponIcon_Worn", "WeaponIcon_Broken"
};
static const char* const TRINKET_TARGETS[4] = {
    "Trinket", "TrinketIcon", "TrinketIcon_Worn", "TrinketIcon_Broken"
};
static const char* const NECK_TARGETS[5] = {
    "NeckItemF", "NeckItemB", "NeckItemIcon", "NeckItemIcon_Worn", "NeckItemIcon_Broken"
};
static const char* const HEAD_ITEM_TARGETS[5] = {
    "HeadItemF", "HeadItemB", "HeadItemIcon", "HeadItemIcon_Worn", "HeadItemIcon_Broken"
};
static const char* const FACE_TARGETS[5] = {
    "FaceItemF", "FaceItemB", "FaceItemIcon", "FaceItemIcon_Worn", "FaceItemIcon_Broken"
};

typedef struct
{
    char    id[MAX_ID_LENGTH];
    char    kind[16];
    char    batch[MAX_ID_LENGTH];
    char    sourcePath[MAX_PATH_LENGTH];
    int32_t logicalIndex;
    int32_t duplicate;
} PartDefinition;

typedef struct
{
    char    batch[MAX_ID_LENGTH];
    char    target[MAX_TARGET_LENGTH];
    int32_t baseFrame;
    int32_t appendedFrames;
    int32_t duplicate;
    int32_t committed;
    void*   destination;
    void*   source;
} BatchTarget;

typedef struct
{
    int   active;
    int   recordBatch;
    void* destination;
    char  batch[MAX_ID_LENGTH];
    char  target[MAX_TARGET_LENGTH];
} PendingAppend;

typedef struct
{
    void* field;
    char  id[MAX_ID_LENGTH];
    char  kind[16];
} PendingNamedField;

static MewjectorAPI                 g_mj;
static HMODULE                      g_moduleHandle;
static fn_process_animation_merges  g_origProcessAnimationMerges;
static fn_find_swf_export           g_origFindSwfExport;
static fn_find_swf_character        g_findSwfCharacter;
static fn_append_movie_clip         g_origAppendMovieClip;
static fn_gon_index_by_name         g_origGonIndexByNameConst;
static fn_gon_index_by_name         g_origGonIndexByName;

static PartDefinition               g_parts[MAX_PARTS];
static BatchTarget                  g_batchTargets[MAX_BATCH_TARGETS];
static PendingNamedField            g_pendingNamedFields[MAX_PENDING_NAMED_FIELDS];
static int32_t                      g_partCount;
static int32_t                      g_batchTargetCount;
static int32_t                      g_pendingNamedFieldCount;

static volatile LONG                g_manifestsLoaded;
static volatile LONG                g_hookInstallState;
static volatile LONG                g_activeAnimationMerges;
static volatile LONG                g_activeAnnotatedAppends;
static CRITICAL_SECTION             g_registryLock;
static CRITICAL_SECTION             g_pendingFieldLock;
static __declspec(thread) PendingAppend g_pendingAppend;
static __declspec(thread) int32_t   g_insideBindingWork;

static void RetryPendingNamedFields(void);
static int  SwfFindExportCharacterId(void* swf, const char* exportName, int32_t* characterId);
static void* FindPaddingCharacterInSwf(void* swf, void* movieClipPrototype, int32_t* foundCharacterId);

static void Log(const char* format, ...)
{
    char buffer[1024];
    va_list args;

    if (!g_mj.Log) return;

    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    g_mj.Log(MOD_NAME, "%s", buffer);
}

static char* TrimInPlace(char* text)
{
    char* end;

    while (text && *text && isspace((unsigned char)*text)) ++text;
    if (!text) return NULL;

    end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) --end;
    *end = '\0';
    return text;
}

static char* StripUtf8Bom(char* text)
{
    unsigned char* bytes = (unsigned char*)text;
    if (bytes && bytes[0] == 0xEFU && bytes[1] == 0xBBU && bytes[2] == 0xBFU)
        return text + 3;
    return text;
}

static int IsMemoryRangeAccessible(const void* address, size_t length, int requireWrite)
{
    MEMORY_BASIC_INFORMATION memory;
    UINT_PTR begin, end, regionEnd;
    DWORD protection;

    if (!address || length == 0U) return 0;

    begin = (UINT_PTR)address;
    end   = begin + length;

    if (end < begin
        || VirtualQuery(address, &memory, sizeof(memory)) != sizeof(memory)
        || memory.State != MEM_COMMIT
        || (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0U)
        return 0;

    regionEnd = (UINT_PTR)memory.BaseAddress + memory.RegionSize;
    if (regionEnd < (UINT_PTR)memory.BaseAddress || end > regionEnd) return 0;
    if (!requireWrite) return 1;

    protection = memory.Protect & 0xFFU;
    return protection == PAGE_READWRITE
        || protection == PAGE_WRITECOPY
        || protection == PAGE_EXECUTE_READWRITE
        || protection == PAGE_EXECUTE_WRITECOPY;
}

static int IsValidId(const char* id)
{
    const unsigned char* cursor = (const unsigned char*)id;
    if (!cursor || *cursor == '\0') return 0;
    while (*cursor)
    {
        if (!isalnum(*cursor) && *cursor != '_' && *cursor != '-' && *cursor != '.')
            return 0;
        ++cursor;
    }
    return 1;
}

static int GetMsvcStringView(const void* stringObject, const char** text, size_t* length)
{
    const uint8_t* bytes = (const uint8_t*)stringObject;
    const char* data;
    size_t size, capacity;

    if (!bytes || !text || !length
        || !IsMemoryRangeAccessible(bytes, MSVC_STRING_CAPACITY_OFFSET + sizeof(capacity), 0))
        return 0;

    memcpy(&size,     bytes + MSVC_STRING_SIZE_OFFSET,     sizeof(size));
    memcpy(&capacity, bytes + MSVC_STRING_CAPACITY_OFFSET, sizeof(capacity));

    if (size > MAX_GON_STRING_LENGTH || capacity < size) return 0;

    data = (capacity <= MSVC_STRING_SSO_CAPACITY) ? (const char*)bytes : NULL;
    if (capacity > MSVC_STRING_SSO_CAPACITY) memcpy(&data, bytes, sizeof(data));

    if (!data || (size != 0U && !IsMemoryRangeAccessible(data, size, 0))) return 0;

    *text   = data;
    *length = size;
    return 1;
}

static int MsvcStringEquals(const void* stringObject, const char* literal)
{
    const char* text;
    size_t length;
    size_t literalLength = strlen(literal);
    return GetMsvcStringView(stringObject, &text, &length)
        && length == literalLength
        && memcmp(text, literal, length) == 0;
}

static int RewriteMsvcStringInPlace(void* stringObject, const char* replacement)
{
    uint8_t* bytes = (uint8_t*)stringObject;
    char* data;
    size_t size, capacity, replacementLength;

    if (!bytes || !replacement
        || !IsMemoryRangeAccessible(bytes, MSVC_STRING_CAPACITY_OFFSET + sizeof(capacity), 1))
        return 0;

    memcpy(&size,     bytes + MSVC_STRING_SIZE_OFFSET,     sizeof(size));
    memcpy(&capacity, bytes + MSVC_STRING_CAPACITY_OFFSET, sizeof(capacity));
    replacementLength = strlen(replacement);

    if (size > MAX_GON_STRING_LENGTH || capacity < size || replacementLength > capacity)
        return 0;

    data = (capacity <= MSVC_STRING_SSO_CAPACITY) ? (char*)bytes : NULL;
    if (capacity > MSVC_STRING_SSO_CAPACITY) memcpy(&data, bytes, sizeof(data));

    if (!data || !IsMemoryRangeAccessible(data, replacementLength + 1U, 1)) return 0;

    memmove(data, replacement, replacementLength);
    data[replacementLength] = '\0';
    memcpy(bytes + MSVC_STRING_SIZE_OFFSET, &replacementLength, sizeof(replacementLength));
    return 1;
}

static int StringViewEqualsLiteral(const char* text, size_t length, const char* literal)
{
    size_t literalLength = strlen(literal);
    return length == literalLength && memcmp(text, literal, length) == 0;
}

static const char* CanonicalKind(const char* kind)
{
    if (_stricmp(kind, "weapon")   == 0) return "weapon";
    if (_stricmp(kind, "trinket")  == 0) return "trinket";
    if (_stricmp(kind, "neckitem") == 0) return "neckItem";
    if (_stricmp(kind, "headitem") == 0) return "headItem";
    if (_stricmp(kind, "faceitem") == 0) return "faceItem";
    return NULL;
}

static const char* KindForGonField(const void* fieldName)
{
    const char* text;
    size_t length;

    if (!GetMsvcStringView(fieldName, &text, &length)) return NULL;
    if (StringViewEqualsLiteral(text, length, "frame")) return "item";
    return NULL;
}

static void GetRequiredTargets(const char* kind,
                               const char* const** targets,
                               int32_t* targetCount)
{
    if (strcmp(kind, "weapon")   == 0) { *targets = WEAPON_TARGETS;    *targetCount = 4; return; }
    if (strcmp(kind, "trinket")  == 0) { *targets = TRINKET_TARGETS;   *targetCount = 4; return; }
    if (strcmp(kind, "neckItem") == 0) { *targets = NECK_TARGETS;      *targetCount = 5; return; }
    if (strcmp(kind, "headItem") == 0) { *targets = HEAD_ITEM_TARGETS; *targetCount = 5; return; }
    if (strcmp(kind, "faceItem") == 0) { *targets = FACE_TARGETS;      *targetCount = 5; return; }

    *targets     = NULL;
    *targetCount = 0;
}

static const char* KindForTarget(const char* target)
{
    int32_t i;
    for (i = 0; i < ITEM_TARGET_GROUP_SIZE; ++i)
    {
        if (_stricmp(target, WEAPON_TARGETS[i])    == 0) return "weapon";
        if (_stricmp(target, TRINKET_TARGETS[i])   == 0) return "trinket";
        if (_stricmp(target, NECK_TARGETS[i])      == 0) return "neckItem";
        if (_stricmp(target, HEAD_ITEM_TARGETS[i]) == 0) return "headItem";
        if (_stricmp(target, FACE_TARGETS[i])      == 0) return "faceItem";
    }
    return NULL;
}

static int ParseManifestLine(char* line, const char* sourcePath, int32_t lineNumber)
{
    char* equals;
    char* left;
    char* right;
    char* context;
    char* token;
    const char* canonicalKind;
    char* end;
    long logicalIndex;
    PartDefinition part;

    line = StripUtf8Bom(TrimInPlace(line));
    if (!line || *line == '\0') return 0;

    equals = strchr(line, '=');
    if (!equals) return 0;

    *equals = '\0';
    left  = TrimInPlace(line);
    right = TrimInPlace(equals + 1);
    memset(&part, 0, sizeof(part));

    if (!IsValidId(left) || strlen(left) >= sizeof(part.id)) return 0;

    snprintf(part.id,         sizeof(part.id),         "%s", left);
    snprintf(part.sourcePath, sizeof(part.sourcePath),  "%s", sourcePath);
    context       = NULL;
    token         = strtok_s(right, " \t,", &context);
    canonicalKind = token ? CanonicalKind(token) : NULL;

    if (!canonicalKind)
    {
        return 0;
    }

    snprintf(part.kind, sizeof(part.kind), "%s", canonicalKind);
    token = strtok_s(NULL, " \t,", &context);

    if (!token || !IsValidId(token) || strlen(token) >= sizeof(part.batch))
    {
        Log("%s:%d: missing or invalid append batch ID", sourcePath, lineNumber);
        return 0;
    }

    snprintf(part.batch, sizeof(part.batch), "%s", token);
    token = strtok_s(NULL, " \t,", &context);

    if (!token) { Log("%s:%d: missing logical part index", sourcePath, lineNumber); return 0; }

    logicalIndex = strtol(token, &end, 10);

    if (*end != '\0' || logicalIndex < 1 || logicalIndex > INT32_MAX)
    {
        Log("%s:%d: logical part index must be a positive integer", sourcePath, lineNumber);
        return 0;
    }

    if (strtok_s(NULL, " \t,", &context) != NULL)
    {
        Log("%s:%d: unexpected data after logical part index", sourcePath, lineNumber);
        return 0;
    }

    part.logicalIndex = (int32_t)logicalIndex;

    if (g_partCount >= MAX_PARTS) { Log("Part registry is full; skipped %s", part.id); return 0; }

    g_parts[g_partCount++] = part;
    return 1;
}

static void LoadManifest(const char* path)
{
    FILE*         file = fopen(path, "rb");
    char          line[MAX_LINE_LENGTH];
    int32_t       lineNumber = 0;
    unsigned char bom[2];

    if (!file) return;

    if (fread(bom, 1U, 2U, file) == 2U
        && ((bom[0] == 0xFFU && bom[1] == 0xFEU) || (bom[0] == 0xFEU && bom[1] == 0xFFU)))
    {
        Log("UTF-16 manifest is unsupported: %s", path);
        fclose(file);
        return;
    }

    fseek(file, 0L, SEEK_SET);
    Log("Reading %s", path);

    while (fgets(line, sizeof(line), file))
    {
        char* comment;
        ++lineNumber;
        comment = strstr(line, "//");
        if (comment) *comment = '\0';
        ParseManifestLine(line, path, lineNumber);
    }

    fclose(file);
}

static void GetDirectoryFromPath(const char* path, char* output, size_t outputSize)
{
    const char* slash = strrchr(path, '\\');
    size_t length;

    if (!slash) { output[0] = '\0'; return; }

    length = (size_t)(slash - path);
    if (length >= outputSize) length = outputSize - 1U;
    memcpy(output, path, length);
    output[length] = '\0';
}

static void GetFileNameFromPath(const char* path, char* output, size_t outputSize)
{
    const char* slash = strrchr(path, '\\');
    snprintf(output, outputSize, "%s", slash ? slash + 1 : path);
}

static int CompareParts(const void* left, const void* right)
{
    const PartDefinition* a = (const PartDefinition*)left;
    const PartDefinition* b = (const PartDefinition*)right;
    int result = _stricmp(a->id, b->id);
    if (result == 0) result = _stricmp(a->sourcePath, b->sourcePath);
    return result;
}

static void FinalizeParts(void)
{
    int32_t index;

    qsort(g_parts, (size_t)g_partCount, sizeof(g_parts[0]), CompareParts);

    for (index = 1; index < g_partCount; ++index)
    {
        if (_stricmp(g_parts[index - 1].id, g_parts[index].id) == 0)
        {
            g_parts[index].duplicate = 1;
            Log("Duplicate @%s ignored from %s, winner is %s",
                g_parts[index].id, g_parts[index].sourcePath,
                g_parts[index - 1].sourcePath);
        }
    }

    for (index = 0; index < g_partCount; ++index)
    {
        if (!g_parts[index].duplicate && g_mj.RegisterName
            && !g_mj.RegisterName("item-frame", g_parts[index].id, MOD_NAME))
        {
            Log("Name collision for item frame @%s", g_parts[index].id);
        }
    }
}

static void ScanSiblingManifests(void)
{
    char             modulePath[MAX_PATH_LENGTH];
    char             frameworkDirectory[MAX_PATH_LENGTH];
    char             frameworkFolder[MAX_PATH_LENGTH];
    char             modsDirectory[MAX_PATH_LENGTH];
    char             searchPath[MAX_PATH_LENGTH];
    WIN32_FIND_DATAA findData;
    HANDLE           findHandle;
    char*            slash;

    if (!GetModuleFileNameA(g_moduleHandle, modulePath, (DWORD)sizeof(modulePath)))
    {
        Log("Could not determine framework DLL path!");
        return;
    }

    GetDirectoryFromPath(modulePath, frameworkDirectory, sizeof(frameworkDirectory));
    GetFileNameFromPath(frameworkDirectory, frameworkFolder, sizeof(frameworkFolder));
    snprintf(modsDirectory, sizeof(modsDirectory), "%s", frameworkDirectory);
    slash = strrchr(modsDirectory, '\\');
    if (!slash) return;

    *slash = '\0';
    snprintf(searchPath, sizeof(searchPath), "%s\\*", modsDirectory);
    findHandle = FindFirstFileA(searchPath, &findData);

    if (findHandle == INVALID_HANDLE_VALUE)
    {
        Log("Could not enumerate sibling mod folders!");
        return;
    }

    do
    {
        if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U
            && strcmp(findData.cFileName, ".") != 0
            && strcmp(findData.cFileName, "..") != 0
            && _stricmp(findData.cFileName, frameworkFolder) != 0)
        {
            char manifestPath[MAX_PATH_LENGTH];
            snprintf(manifestPath, sizeof(manifestPath),
                     "%s\\%s\\cat_parts.txt", modsDirectory, findData.cFileName);

            if (GetFileAttributesA(manifestPath) != INVALID_FILE_ATTRIBUTES)
                LoadManifest(manifestPath);
        }
    } while (FindNextFileA(findHandle, &findData));

    FindClose(findHandle);

    snprintf(searchPath, sizeof(searchPath), "%s\\cat_parts.txt", frameworkDirectory);
    if (GetFileAttributesA(searchPath) != INVALID_FILE_ATTRIBUTES)
        LoadManifest(searchPath);
}

static void EnsureManifestsLoaded(void)
{
    LONG state = InterlockedCompareExchange(&g_manifestsLoaded, 1, 0);

    if (state == 0)
    {
        ScanSiblingManifests();
        FinalizeParts();
        MemoryBarrier();
        InterlockedExchange(&g_manifestsLoaded, 2);
        Log("Item part manifest scan complete: %d definitions", g_partCount);
        return;
    }

    while (InterlockedCompareExchange(&g_manifestsLoaded, 2, 2) == 1) Sleep(0);
}

static int ParseAnnotatedExport(const void* nameObject,
                                char* target, size_t targetSize,
                                char* batch,  size_t batchSize)
{
    static const char marker[] = MIF_MARKER;
    const size_t markerLength  = sizeof(marker) - 1U;
    const char* text;
    const char* markerAt = NULL;
    size_t index, length, targetLength, batchLength;

    if (!GetMsvcStringView(nameObject, &text, &length)) return 0;
    if (length < markerLength) return 0;

    for (index = 0; index + markerLength <= length; ++index)
    {
        if (memcmp(text + index, marker, markerLength) == 0)
        {
            markerAt = text + index;
            break;
        }
    }

    if (!markerAt) return 0;

    targetLength = (size_t)(markerAt - text);
    batchLength  = length - targetLength - markerLength;

    if (targetLength == 0U || targetLength >= targetSize
        || batchLength == 0U || batchLength >= batchSize)
        return 0;

    memcpy(target, text, targetLength); target[targetLength] = '\0';
    memcpy(batch, markerAt + markerLength, batchLength); batch[batchLength] = '\0';

    return IsValidId(batch);
}

static int32_t MovieClipFrameCount(void* clip)
{
    uint8_t* bytes = (uint8_t*)clip;
    void*    data;
    int32_t  frameCount;

    if (!bytes || !IsMemoryRangeAccessible(bytes, MOVIE_CLIP_DATA_OFFSET + sizeof(data), 0))
        return -1;

    memcpy(&data, bytes + MOVIE_CLIP_DATA_OFFSET, sizeof(data));
    if (!data) data = bytes + MOVIE_CLIP_INLINE_DATA_OFFSET;

    if (!IsMemoryRangeAccessible(data, MOVIE_CLIP_FRAME_COUNT_OFFSET + sizeof(frameCount), 0))
        return -1;

    memcpy(&frameCount, (uint8_t*)data + MOVIE_CLIP_FRAME_COUNT_OFFSET, sizeof(frameCount));
    return frameCount;
}

static int SwfFindExportCharacterId(void* swf, const char* exportName, int32_t* characterId)
{
    uint8_t* bytes = (uint8_t*)swf;
    void*    sentinel;
    void*    node;
    int32_t  iterations;

    if (!swf || !exportName || !characterId
        || !IsMemoryRangeAccessible(bytes + SWF_EXPORT_LIST_OFFSET, sizeof(sentinel), 0))
        return 0;

    memcpy(&sentinel, bytes + SWF_EXPORT_LIST_OFFSET, sizeof(sentinel));
    if (!sentinel || !IsMemoryRangeAccessible(sentinel, sizeof(node), 0)) return 0;
    memcpy(&node, sentinel, sizeof(node));

    for (iterations = 0; node && node != sentinel && iterations < 4096; ++iterations)
    {
        uint8_t* nodeBytes = (uint8_t*)node;
        int32_t  id;
        void*    next;

        if (!IsMemoryRangeAccessible(node, SWF_EXPORT_NODE_CHARACTER_ID_OFFSET + sizeof(id), 0))
            return 0;

        if (MsvcStringEquals(nodeBytes + SWF_EXPORT_NODE_NAME_OFFSET, exportName))
        {
            memcpy(&id, nodeBytes + SWF_EXPORT_NODE_CHARACTER_ID_OFFSET, sizeof(id));
            if (id <= 0) return 0;
            *characterId = id;
            return 1;
        }

        memcpy(&next, node, sizeof(next));
        node = next;
    }

    return 0;
}

static void* FindExportInSwf(void* swf, const char* exportName)
{
    int32_t characterId;

    if (!swf || !exportName || !g_findSwfCharacter
        || !SwfFindExportCharacterId(swf, exportName, &characterId))
        return NULL;

    return g_findSwfCharacter((uint8_t*)swf + SWF_CHARACTER_TABLE_OFFSET, characterId);
}

static void* FindOwningSwfForExport(void* application, const char* exportName, void* expectedExport)
{
    uint8_t* bytes = (uint8_t*)application;
    void**   swfs;
    int32_t  swfCount;
    int32_t  index;

    if (!application || !exportName || !expectedExport || !g_findSwfCharacter
        || !IsMemoryRangeAccessible(bytes + APPLICATION_SWF_COUNT_OFFSET,
               APPLICATION_SWF_ARRAY_OFFSET - APPLICATION_SWF_COUNT_OFFSET + sizeof(swfs), 0))
        return NULL;

    memcpy(&swfCount, bytes + APPLICATION_SWF_COUNT_OFFSET, sizeof(swfCount));
    memcpy(&swfs,     bytes + APPLICATION_SWF_ARRAY_OFFSET, sizeof(swfs));

    if (swfCount <= 0 || swfCount > MAX_APPLICATION_SWFS || !swfs
        || !IsMemoryRangeAccessible(swfs, (size_t)swfCount * sizeof(*swfs), 0))
        return NULL;

    for (index = swfCount - 1; index >= 0; --index)
    {
        void*   swf = swfs[index];
        int32_t characterId;
        void*   candidate;

        if (!SwfFindExportCharacterId(swf, exportName, &characterId)) continue;
        candidate = g_findSwfCharacter((uint8_t*)swf + SWF_CHARACTER_TABLE_OFFSET, characterId);
        if (candidate == expectedExport) return swf;
    }

    return NULL;
}

static int MovieClipDefinitionTypeMatches(void* left, void* right)
{
    void* leftType;
    void* rightType;

    if (!left || !right
        || !IsMemoryRangeAccessible(left,  sizeof(leftType),  0)
        || !IsMemoryRangeAccessible(right, sizeof(rightType), 0))
        return 0;

    memcpy(&leftType,  left,  sizeof(leftType));
    memcpy(&rightType, right, sizeof(rightType));
    return leftType != NULL && leftType == rightType;
}

static void* FindPaddingCharacterInSwf(void* swf, void* movieClipPrototype, int32_t* foundCharacterId)
{
    int32_t characterId;
    void*   filler;

    if (foundCharacterId) *foundCharacterId = -1;
    if (!swf || !movieClipPrototype || !g_findSwfCharacter) return NULL;

    filler = g_findSwfCharacter((uint8_t*)swf + SWF_CHARACTER_TABLE_OFFSET, 3);
    if (filler && filler != movieClipPrototype && MovieClipFrameCount(filler) == 1
        && MovieClipDefinitionTypeMatches(filler, movieClipPrototype))
    {
        if (foundCharacterId) *foundCharacterId = 3;
        return filler;
    }

    for (characterId = 1; characterId <= 32768; ++characterId)
    {
        if (characterId == 3) continue;
        filler = g_findSwfCharacter((uint8_t*)swf + SWF_CHARACTER_TABLE_OFFSET, characterId);

        if (!filler || filler == movieClipPrototype) continue;

        if (MovieClipFrameCount(filler) == 1
            && MovieClipDefinitionTypeMatches(filler, movieClipPrototype))
        {
            if (foundCharacterId) *foundCharacterId = characterId;
            return filler;
        }
    }

    return NULL;
}

static BatchTarget* FindBatchTarget(const char* batch, const char* target)
{
    int32_t index;
    for (index = 0; index < g_batchTargetCount; ++index)
    {
        if (_stricmp(g_batchTargets[index].batch, batch) == 0
            && _stricmp(g_batchTargets[index].target, target) == 0)
            return &g_batchTargets[index];
    }
    return NULL;
}

static void PersistBatchTargetRange(const char* batch, const char* target,
                                    int32_t baseFrame, int32_t appendedFrames)
{
    char  modulePath[MAX_PATH_LENGTH];
    char  frameworkDirectory[MAX_PATH_LENGTH];
    char  registryPath[MAX_PATH_LENGTH];
    FILE* registryFile;
    long  existingBytes;

    if (!batch || !target || appendedFrames <= 0 || baseFrame < 0) return;
    if (!GetModuleFileNameA(g_moduleHandle, modulePath, (DWORD)sizeof(modulePath))) return;

    GetDirectoryFromPath(modulePath, frameworkDirectory, sizeof(frameworkDirectory));
    snprintf(registryPath, sizeof(registryPath),
             "%s\\item_frame_ranges.tsv", frameworkDirectory);
    registryFile = fopen(registryPath, "ab+");
    if (!registryFile) return;

    if (fseek(registryFile, 0, SEEK_END) == 0)
    {
        existingBytes = ftell(registryFile);
        if (existingBytes == 0)
        {
            fputs("# MewItemFramework custom append range registry v1\r\n", registryFile);
            fputs("# magic\tbatch\ttarget\tfirstFrame\tlastFrame\r\n", registryFile);
        }
    }

    fprintf(registryFile, "MIF1\t%s\t%s\t%d\t%d\r\n",
            batch, target, baseFrame + 1, baseFrame + appendedFrames);
    fflush(registryFile);
    fclose(registryFile);
}

static void RecordBatchTarget(const char* batch, const char* target,
                              int32_t baseFrame, int32_t appendedFrames,
                              void* destination, void* source)
{
    BatchTarget* existing;
    BatchTarget* added;

    EnterCriticalSection(&g_registryLock);
    PersistBatchTargetRange(batch, target, baseFrame, appendedFrames);
    existing = FindBatchTarget(batch, target);

    if (existing)
    {
        existing->duplicate = 1;
        Log("Duplicate append batch target: %s / %s; named parts in this batch are disabled",
            batch, target);
        LeaveCriticalSection(&g_registryLock);
        return;
    }

    if (g_batchTargetCount >= MAX_BATCH_TARGETS)
    {
        Log("Append batch registry is full, skipped %s / %s", batch, target);
        LeaveCriticalSection(&g_registryLock);
        return;
    }

    added = &g_batchTargets[g_batchTargetCount++];
    memset(added, 0, sizeof(*added));
    snprintf(added->batch,  sizeof(added->batch),  "%s", batch);
    snprintf(added->target, sizeof(added->target), "%s", target);
    added->baseFrame      = baseFrame;
    added->appendedFrames = appendedFrames;
    added->destination    = destination;
    added->source         = source;
    Log("Bound batch %s / %s to frames %d..%d",
        batch, target, baseFrame + 1, baseFrame + appendedFrames);
    LeaveCriticalSection(&g_registryLock);
}

static void MarkBatchTargetCommitted(const char* batch, const char* target)
{
    BatchTarget* mapping;
    EnterCriticalSection(&g_registryLock);
    mapping = FindBatchTarget(batch, target);
    if (mapping && !mapping->duplicate) mapping->committed = 1;
    LeaveCriticalSection(&g_registryLock);
}

static int BatchAlreadyHasAnyTarget(const char* batch,
                                    const char* const* targets, int32_t targetCount)
{
    int32_t index;
    int     found = 0;

    EnterCriticalSection(&g_registryLock);
    for (index = 0; index < targetCount; ++index)
    {
        if (FindBatchTarget(batch, targets[index])) { found = 1; break; }
    }
    LeaveCriticalSection(&g_registryLock);
    return found;
}

static int AlignTargetGroupBeforeFirstBatchAppend(
    void* application,
    const char* batch,
    const char* const* targets,
    int32_t targetCount,
    const char* groupName,
    const char* anchorTarget,
    void* anchorDestination)
{
    void*   destinations[ITEM_TARGET_GROUP_SIZE];
    void*   ownerSwf;
    void*   filler = NULL;
    int32_t frames[ITEM_TARGET_GROUP_SIZE];
    int32_t index;
    int32_t canonicalBase = -1;

    if (!application || !batch || !targets || targetCount < 2
        || targetCount > ITEM_TARGET_GROUP_SIZE
        || !groupName || !anchorTarget || !anchorDestination || !g_origAppendMovieClip)
        return 0;

    if (BatchAlreadyHasAnyTarget(batch, targets, targetCount)) return 1;

    ownerSwf = FindOwningSwfForExport(application, anchorTarget, anchorDestination);
    if (!ownerSwf)
    {
        Log("Cannot pre-align batch %s / %s: native destination %s has no owning SWF",
            batch, groupName, anchorTarget);
        return 0;
    }

    for (index = 0; index < targetCount; ++index)
    {
        destinations[index] = FindExportInSwf(ownerSwf, targets[index]);
        frames[index]       = MovieClipFrameCount(destinations[index]);

        if (!destinations[index] || frames[index] < 1 || frames[index] > 100000)
        {
            Log("Cannot pre-align batch %s / %s: destination %s has invalid frame count %d",
                batch, groupName, targets[index], frames[index]);
            return 0;
        }

        if (frames[index] > canonicalBase) canonicalBase = frames[index];
    }

    for (index = 0; index < targetCount; ++index)
    {
        void*   destClip      = destinations[index];
        int32_t paddingFrames = canonicalBase - frames[index];
        int32_t paddingIndex;

        if (paddingFrames <= 0 || !destClip) continue;

        if (!filler)
        {
            int32_t fillerCharacterId = -1;
            filler = FindPaddingCharacterInSwf(ownerSwf, destClip, &fillerCharacterId);
            if (!filler)
            {
                Log("Cannot pre-align batch %s / %s: no compatible 1-frame filler found in SWF",
                    batch, groupName);
                return 0;
            }
            Log("Pre-align batch %s / %s: discovered compatible one-frame character %d",
                batch, groupName, fillerCharacterId);
        }

        for (paddingIndex = 0; paddingIndex < paddingFrames; ++paddingIndex)
            g_origAppendMovieClip(destClip, filler);

        if (MovieClipFrameCount(destClip) != canonicalBase)
        {
            Log("Cannot pre-align batch %s / %s: %s stopped at frame %d instead of %d",
                batch, groupName, targets[index],
                MovieClipFrameCount(destClip), canonicalBase);
            return 0;
        }
    }

    Log("Pre-aligned batch %s / %s base timelines at frame %d", batch, groupName, canonicalBase);
    return 1;
}

static int AlignNamedTargetBeforeAppend(void* application, const char* batch,
                                        const char* target, void* nativeDestination)
{
    const char*        kind    = KindForTarget(target);
    const char* const* targets = NULL;
    int32_t            count   = 0;

    if (!kind) return 1;

    GetRequiredTargets(kind, &targets, &count);

    if (!targets || count < 2) return 1;

    return AlignTargetGroupBeforeFirstBatchAppend(
        application, batch, targets, count, kind, target, nativeDestination);
}

static PartDefinition* FindPart(const char* id)
{
    int32_t low  = 0;
    int32_t high = g_partCount - 1;

    while (low <= high)
    {
        int32_t middle = low + (high - low) / 2;
        int result = _stricmp(id, g_parts[middle].id);

        if (result == 0)
        {
            while (middle > 0 && _stricmp(id, g_parts[middle - 1].id) == 0) --middle;
            return g_parts[middle].duplicate ? NULL : &g_parts[middle];
        }

        if (result < 0) high = middle - 1;
        else            low  = middle + 1;
    }

    return NULL;
}

static int ResolvePartFrame(const PartDefinition* part, int32_t* resolvedFrame)
{
    const char* const* requiredTargets = NULL;
    int32_t            requiredCount   = 0;
    int32_t            index;
    int32_t            sharedFirst = INT32_MIN;
    int32_t            sharedLast  = INT32_MAX;
    int64_t            resolved;

    if (!part || !resolvedFrame) return 0;

    GetRequiredTargets(part->kind, &requiredTargets, &requiredCount);
    if (!requiredTargets || requiredCount < 1) return 0;

    EnterCriticalSection(&g_registryLock);

    for (index = 0; index < requiredCount; ++index)
    {
        BatchTarget* mapping = FindBatchTarget(part->batch, requiredTargets[index]);
        int64_t firstFrame, lastFrame;

        if (!mapping || mapping->duplicate || mapping->appendedFrames < 1)
        {
            LeaveCriticalSection(&g_registryLock);
            return 0;
        }

        firstFrame = (int64_t)mapping->baseFrame + 1;
        lastFrame  = (int64_t)mapping->baseFrame + (int64_t)mapping->appendedFrames;

        if (firstFrame < 1 || lastFrame < firstFrame || lastFrame > INT32_MAX)
        {
            LeaveCriticalSection(&g_registryLock);
            return 0;
        }

        if ((int32_t)firstFrame > sharedFirst) sharedFirst = (int32_t)firstFrame;
        if ((int32_t)lastFrame  < sharedLast)  sharedLast  = (int32_t)lastFrame;
    }

    LeaveCriticalSection(&g_registryLock);

    if (sharedFirst > sharedLast)
    {
        Log("@%s cannot resolve: companion append ranges do not overlap (%d..%d)",
            part->id, sharedFirst, sharedLast);
        return 0;
    }

    resolved = (int64_t)sharedFirst + (int64_t)part->logicalIndex - 1;

    if (resolved < sharedFirst || resolved > sharedLast || resolved > INT32_MAX)
    {
        Log("@%s logical index %d is outside the observed append range %d..%d",
            part->id, part->logicalIndex, sharedFirst, sharedLast);
        return 0;
    }

    *resolvedFrame = (int32_t)resolved;
    return 1;
}

static int BindingWorkIsActive(void)
{
    return InterlockedCompareExchange(&g_activeAnimationMerges, 0, 0) != 0
        || InterlockedCompareExchange(&g_activeAnnotatedAppends, 0, 0) != 0;
}

static int ResolvePartFrameAfterActiveBindings(const PartDefinition* part, int32_t* resolvedFrame)
{
    int32_t yields;

    if (ResolvePartFrame(part, resolvedFrame)) return 1;
    if (g_insideBindingWork != 0) return 0;

    for (yields = 0; yields < BINDING_WAIT_YIELD_COUNT && BindingWorkIsActive(); ++yields)
        Sleep(0);

    return ResolvePartFrame(part, resolvedFrame);
}

static int ReadNamedPartId(void* field, char* id, size_t idSize)
{
    uint8_t*    bytes = (uint8_t*)field;
    const char* token;
    size_t      tokenLength;
    int32_t     fieldType;

    if (!field || !id || idSize < 2U
        || !IsMemoryRangeAccessible(field, GON_TYPE_OFFSET + sizeof(fieldType), 0))
        return 0;

    memcpy(&fieldType, bytes + GON_TYPE_OFFSET, sizeof(fieldType));

    if (fieldType != GON_TYPE_STRING
        || !GetMsvcStringView(bytes + GON_STRING_DATA_OFFSET, &token, &tokenLength)
        || tokenLength < 2U || tokenLength >= idSize || token[0] != '@')
        return 0;

    memcpy(id, token + 1, tokenLength - 1U);
    id[tokenLength - 1U] = '\0';
    return IsValidId(id);
}

static int ApplyResolvedPartFrame(void* field, int32_t frame)
{
    uint8_t* bytes     = (uint8_t*)field;
    int32_t  fieldType = GON_TYPE_NUMBER;
    double   frameAsDouble = (double)frame;

    if (!IsMemoryRangeAccessible(field, GON_TYPE_OFFSET + sizeof(fieldType), 1)) return 0;

    memcpy(bytes + GON_INT_DATA_OFFSET,   &frame,         sizeof(frame));
    memcpy(bytes + GON_FLOAT_DATA_OFFSET, &frameAsDouble, sizeof(frameAsDouble));
    MemoryBarrier();
    memcpy(bytes + GON_TYPE_OFFSET, &fieldType, sizeof(fieldType));
    return 1;
}

static void QueuePendingNamedField(void* field, const char* id, const char* kind)
{
    int32_t           index;
    PendingNamedField* pending;

    EnterCriticalSection(&g_pendingFieldLock);

    for (index = 0; index < g_pendingNamedFieldCount; ++index)
    {
        pending = &g_pendingNamedFields[index];
        if (pending->field == field)
        {
            snprintf(pending->id,   sizeof(pending->id),   "%s", id);
            snprintf(pending->kind, sizeof(pending->kind), "%s", kind);
            LeaveCriticalSection(&g_pendingFieldLock);
            return;
        }
    }

    if (g_pendingNamedFieldCount >= MAX_PENDING_NAMED_FIELDS)
    {
        LeaveCriticalSection(&g_pendingFieldLock);
        Log("Pending named item-frame registry is full, skipped @%s", id);
        return;
    }

    pending = &g_pendingNamedFields[g_pendingNamedFieldCount++];
    memset(pending, 0, sizeof(*pending));
    pending->field = field;
    snprintf(pending->id,   sizeof(pending->id),   "%s", id);
    snprintf(pending->kind, sizeof(pending->kind), "%s", kind);
    LeaveCriticalSection(&g_pendingFieldLock);
    Log("Deferred @%s until its %s companion appends are ready", id, kind);
}

static int RetryPendingNamedField(const PendingNamedField* pending)
{
    char            currentId[MAX_ID_LENGTH];
    PartDefinition* part;
    int32_t         frame;

    if (!ReadNamedPartId(pending->field, currentId, sizeof(currentId))
        || _stricmp(currentId, pending->id) != 0)
        return -1;

    EnsureManifestsLoaded();
    part = FindPart(currentId);

    if (!part || strcmp(part->kind, pending->kind) != 0) return -1;
    if (!ResolvePartFrame(part, &frame)) return 0;
    if (!ApplyResolvedPartFrame(pending->field, frame)) return -1;

    Log("Resolved deferred @%s => frame %d", currentId, frame);
    return 1;
}

static void RetryPendingNamedFields(void)
{
    PendingNamedField pending;
    int32_t           index = 0;
    int               status;

    while (1)
    {
        EnterCriticalSection(&g_pendingFieldLock);
        if (index >= g_pendingNamedFieldCount) { LeaveCriticalSection(&g_pendingFieldLock); break; }
        pending = g_pendingNamedFields[index];
        LeaveCriticalSection(&g_pendingFieldLock);

        status = RetryPendingNamedField(&pending);

        EnterCriticalSection(&g_pendingFieldLock);
        if (index < g_pendingNamedFieldCount
            && g_pendingNamedFields[index].field == pending.field
            && _stricmp(g_pendingNamedFields[index].id, pending.id) == 0)
        {
            if (status != 0)
            {
                --g_pendingNamedFieldCount;
                g_pendingNamedFields[index] = g_pendingNamedFields[g_pendingNamedFieldCount];
            }
            else
            {
                ++index;
            }
        }
        LeaveCriticalSection(&g_pendingFieldLock);
    }
}

static int FieldLooksLikeNamedPartFast(const void* field)
{
    const uint8_t* bytes = (const uint8_t*)field;
    const uint8_t* stringBytes;
    const char*    text;
    size_t         size, capacity;
    int32_t        type;

    if (!bytes) return 0;

    memcpy(&type, bytes + GON_TYPE_OFFSET, sizeof(type));
    if (type != GON_TYPE_STRING) return 0;

    stringBytes = bytes + GON_STRING_DATA_OFFSET;
    memcpy(&size,     stringBytes + MSVC_STRING_SIZE_OFFSET,     sizeof(size));
    memcpy(&capacity, stringBytes + MSVC_STRING_CAPACITY_OFFSET, sizeof(capacity));

    if (size < 2U || size > MAX_GON_STRING_LENGTH || capacity < size) return 0;

    text = (capacity <= MSVC_STRING_SSO_CAPACITY) ? (const char*)stringBytes : NULL;
    if (capacity > MSVC_STRING_SSO_CAPACITY) memcpy(&text, stringBytes, sizeof(text));

    return text && text[0] == '@';
}

static void MaybeResolveNamedPart(void* field, const char* expectedKind)
{
    char            id[MAX_ID_LENGTH];
    int32_t         frame;
    PartDefinition* part;

    (void)expectedKind;

    if (!ReadNamedPartId(field, id, sizeof(id))) return;

    EnsureManifestsLoaded();
    part = FindPart(id);

    if (!part)
    {
        Log("Unknown named item frame: @%s", id);
        return;
    }

    if (!ResolvePartFrameAfterActiveBindings(part, &frame))
    {
        QueuePendingNamedField(field, id, part->kind);
        return;
    }

    if (ApplyResolvedPartFrame(field, frame))
        Log("Resolved @%s => frame %d", id, frame);
}

static void* __fastcall HookFindSwfExport(void* application, void* name)
{
    char  target[MAX_TARGET_LENGTH];
    char  batch[MAX_ID_LENGTH];
    void* result;

    memset(&g_pendingAppend, 0, sizeof(g_pendingAppend));

    if (ParseAnnotatedExport(name, target, sizeof(target), batch, sizeof(batch)))
    {
        if (!RewriteMsvcStringInPlace(name, target))
        {
            Log("Cannot rewrite annotated append target %s", target);
            return g_origFindSwfExport ? g_origFindSwfExport(application, name) : NULL;
        }

        result = g_origFindSwfExport ? g_origFindSwfExport(application, name) : NULL;

        if (result && !AlignNamedTargetBeforeAppend(application, batch, target, result))
            Log("Named append %s / %s will continue without companion alignment", batch, target);

        if (result)
        {
            g_pendingAppend.active      = 1;
            g_pendingAppend.recordBatch = 1;
            g_pendingAppend.destination = result;
            snprintf(g_pendingAppend.batch,  sizeof(g_pendingAppend.batch),  "%s", batch);
            snprintf(g_pendingAppend.target, sizeof(g_pendingAppend.target), "%s", target);
        }

        return result;
    }

    return g_origFindSwfExport ? g_origFindSwfExport(application, name) : NULL;
}

static void __fastcall HookAppendMovieClip(void* destination, void* source)
{
    PendingAppend pending        = g_pendingAppend;
    int32_t       baseFrame      = -1;
    int32_t       appendedFrames = -1;
    int           recordBatch    = 0;

    memset(&g_pendingAppend, 0, sizeof(g_pendingAppend));

    if (pending.active && pending.destination == destination)
    {
        baseFrame      = MovieClipFrameCount(destination);
        appendedFrames = MovieClipFrameCount(source);
        recordBatch    = pending.recordBatch;
    }

    if (recordBatch && baseFrame >= 0 && appendedFrames > 0)
    {
        InterlockedIncrement(&g_activeAnnotatedAppends);
        ++g_insideBindingWork;
        RecordBatchTarget(pending.batch, pending.target,
                          baseFrame, appendedFrames, destination, source);
    }

    if (g_origAppendMovieClip) g_origAppendMovieClip(destination, source);

    if (recordBatch && baseFrame >= 0 && appendedFrames > 0)
    {
        MarkBatchTargetCommitted(pending.batch, pending.target);
        --g_insideBindingWork;

        if (InterlockedDecrement(&g_activeAnnotatedAppends) == 0
            && InterlockedCompareExchange(&g_activeAnimationMerges, 0, 0) == 0)
            RetryPendingNamedFields();
    }
}

static void __fastcall HookProcessAnimationMerges(void* application, void* swf)
{
    LONG state;

    do
    {
        state = InterlockedCompareExchange(&g_hookInstallState, 0, 0);
        if (state == 1) Sleep(0);
    } while (state == 1);

    if (g_origProcessAnimationMerges)
    {
        InterlockedIncrement(&g_activeAnimationMerges);
        ++g_insideBindingWork;
        g_origProcessAnimationMerges(application, swf);
        --g_insideBindingWork;

        if (InterlockedDecrement(&g_activeAnimationMerges) == 0
            && InterlockedCompareExchange(&g_activeAnnotatedAppends, 0, 0) == 0)
            RetryPendingNamedFields();
    }
}

static void* __fastcall HookGonIndexByNameConst(void* gonObject, const void* fieldName)
{
    void*       field = g_origGonIndexByNameConst
                        ? g_origGonIndexByNameConst(gonObject, fieldName) : NULL;
    const char* kind;

    if (!FieldLooksLikeNamedPartFast(field)) return field;

    kind = KindForGonField(fieldName);
    if (kind) MaybeResolveNamedPart(field, kind);

    return field;
}

static void* __fastcall HookGonIndexByName(void* gonObject, const void* fieldName)
{
    void*       field = g_origGonIndexByName
                        ? g_origGonIndexByName(gonObject, fieldName) : NULL;
    const char* kind;

    if (!FieldLooksLikeNamedPartFast(field)) return field;

    kind = KindForGonField(fieldName);
    if (kind) MaybeResolveNamedPart(field, kind);

    return field;
}

__declspec(dllexport) int __cdecl
MewItemFramework_ResolvePart(const char* id, int32_t* resolvedFrame)
{
    PartDefinition* part;

    if (!id || !resolvedFrame) return 0;
    if (*id == '@') ++id;
    if (!IsValidId(id)) return 0;

    EnsureManifestsLoaded();
    part = FindPart(id);
    if (!part) return 0;

    return ResolvePartFrameAfterActiveBindings(part, resolvedFrame);
}

static int InstallHooks(void)
{
    LONG     state;
    UINT_PTR base;
    void*    trampoline = NULL;

    state = InterlockedCompareExchange(&g_hookInstallState, 1, 0);
    if (state != 0) return state == 1 || state == 2;

    if (!MJ_Resolve(&g_mj)) { InterlockedExchange(&g_hookInstallState, 0); return 0; }

    base = g_mj.GetGameBase();
    if (!base)
    {
        Log("Failed to get base address!");
        InterlockedExchange(&g_hookInstallState, 3);
        return 0;
    }

    g_findSwfCharacter = (fn_find_swf_character)(base + RVA_FIND_SWF_CHARACTER);

    if (!g_mj.InstallHook(RVA_PROCESS_ANIMATION_MERGES, PROCESS_MERGES_HOOK_STOLEN_BYTES,
                           (void*)HookProcessAnimationMerges, &trampoline, 5, MOD_NAME))
    {
        Log("Failed to install animation-merge startup gate");
        InterlockedExchange(&g_hookInstallState, 3);
        return 0;
    }
    g_origProcessAnimationMerges = (fn_process_animation_merges)trampoline;
    trampoline = NULL;

    if (!g_mj.InstallHook(RVA_FIND_SWF_EXPORT, FIND_EXPORT_HOOK_STOLEN_BYTES,
                           (void*)HookFindSwfExport, &trampoline, 10, MOD_NAME))
    {
        Log("Failed to install annotated-export hook");
        InterlockedExchange(&g_hookInstallState, 3);
        return 0;
    }
    g_origFindSwfExport = (fn_find_swf_export)trampoline;
    trampoline = NULL;

    if (!g_mj.InstallHook(RVA_APPEND_MOVIE_CLIP, APPEND_CLIP_HOOK_STOLEN_BYTES,
                           (void*)HookAppendMovieClip, &trampoline, 10, MOD_NAME))
    {
        Log("Failed to install MovieClip append hook");
        InterlockedExchange(&g_hookInstallState, 3);
        return 0;
    }
    g_origAppendMovieClip = (fn_append_movie_clip)trampoline;
    trampoline = NULL;

    if (!g_mj.InstallHook(RVA_GON_INDEX_BY_NAME_CONST, GON_INDEX_HOOK_STOLEN_BYTES,
                           (void*)HookGonIndexByNameConst, &trampoline, 20, MOD_NAME))
    {
        Log("Failed to install const GON hook");
        InterlockedExchange(&g_hookInstallState, 3);
        return 0;
    }
    g_origGonIndexByNameConst = (fn_gon_index_by_name)trampoline;
    trampoline = NULL;

    if (!g_mj.InstallHook(RVA_GON_INDEX_BY_NAME, GON_INDEX_HOOK_STOLEN_BYTES,
                           (void*)HookGonIndexByName, &trampoline, 20, MOD_NAME))
    {
        Log("Failed to install mutable GON hook");
        InterlockedExchange(&g_hookInstallState, 3);
        return 0;
    }
    g_origGonIndexByName = (fn_gon_index_by_name)trampoline;
    MemoryBarrier();
    InterlockedExchange(&g_hookInstallState, 2);
    Log("Installed!");
    return 1;
}

__declspec(dllexport) void __cdecl MewItemFramework_Init(void)
{
    InstallHooks();
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved)
{
    (void)reserved;

    if (reason == DLL_PROCESS_ATTACH)
    {
        g_moduleHandle = module;
        DisableThreadLibraryCalls(module);
        InitializeCriticalSection(&g_registryLock);
        InitializeCriticalSection(&g_pendingFieldLock);
        MJ_Resolve(&g_mj);
        InstallHooks();
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        DeleteCriticalSection(&g_pendingFieldLock);
        DeleteCriticalSection(&g_registryLock);
    }

    return TRUE;
}
