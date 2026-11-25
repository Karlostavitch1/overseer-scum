#include "ue4/reflection.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <map>

#include "core/log.h"
#include "util/module.h"
#include "util/safe_memory.h"

namespace overseer::ue4 {
namespace {

using memory::Read;

constexpr int32_t kChunkSize = 64 * 1024;     // FChunkedFixedUObjectArray::NumElementsPerChunk
constexpr uint32_t kObjectItemSize = 0x18;    // FUObjectItem: Object, Flags, ClusterRootIndex, SerialNumber
constexpr uint32_t kNameStride = 2;           // alignof(FNameEntry)
constexpr uint32_t kFuncNative = 0x00000400;  // EFunctionFlags::FUNC_Native

bool PlausiblePointer(uintptr_t p) { return p >= 0x10000 && p < 0x00007FFFFFFFFFFF; }

// Caches the last readable region so scanning millions of candidate
// pointers doesn't cost a VirtualQuery (or an exception) each.
class ReadableCache {
public:
    bool Check(uintptr_t address, size_t size) {
        if (address >= begin_ && address + size <= end_) return true;
        MEMORY_BASIC_INFORMATION info;
        if (!VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info))) return false;
        const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                               PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        if (info.State != MEM_COMMIT || !(info.Protect & readable) || (info.Protect & PAGE_GUARD)) {
            return false;
        }
        begin_ = reinterpret_cast<uintptr_t>(info.BaseAddress);
        end_ = begin_ + info.RegionSize;
        return address + size <= end_;
    }

private:
    uintptr_t begin_ = 0, end_ = 0;
};

template <typename Fn>
void ForEachQword(std::span<const uint8_t> section, Fn&& fn) {
    auto begin = reinterpret_cast<uintptr_t>(section.data());
    begin = (begin + 7) & ~uintptr_t{7};
    const uintptr_t end = reinterpret_cast<uintptr_t>(section.data()) + section.size();
    for (uintptr_t at = begin; at + 8 <= end; at += 8) {
        if (fn(at)) return;
    }
}

std::string Hex(uintptr_t value) {
    char buf[24];
    snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(value));
    return buf;
}

}  // namespace

// --- Names ------------------------------------------------------------------

bool Reflection::FindNamePool(std::string& error) {
    auto data = module::Section(GetModuleHandleW(nullptr), ".data");
    if (!data) {
        error = "no .data section";
        return false;
    }

    // FNamePool::Entries.Blocks[0] points at the first name block, which
    // always starts with the entries "None" and "ByteProperty". Each entry is
    // a uint16 header (bIsWide:1, LowercaseProbeHash:5, Len:10) followed by
    // the characters, padded to 2 bytes.
    ReadableCache cache;
    ForEachQword(*data, [&](uintptr_t slot) {
        uintptr_t block = *reinterpret_cast<const uintptr_t*>(slot);
        if (!PlausiblePointer(block) || (block & 1) || !cache.Check(block, 20)) return false;
        uint8_t bytes[20];
        if (!memory::SafeRead(reinterpret_cast<const void*>(block), bytes, sizeof(bytes))) return false;
        uint16_t h0, h1;
        std::memcpy(&h0, bytes, 2);
        std::memcpy(&h1, bytes + 6, 2);
        if ((h0 & 1) || (h0 >> 6) != 4 || std::memcmp(bytes + 2, "None", 4) != 0) return false;
        if ((h1 & 1) || (h1 >> 6) != 12 || std::memcmp(bytes + 8, "ByteProperty", 12) != 0) return false;
        nameBlocks_ = slot;
        return true;
    });

    if (!nameBlocks_) {
        error = "FNamePool not found";
        return false;
    }
    return true;
}

std::string Reflection::NameToString(FName name) const {
    if (!nameBlocks_ || name.comparisonIndex < 0) return {};
    const uint32_t index = static_cast<uint32_t>(name.comparisonIndex);
    uintptr_t block = 0;
    if (!Read(nameBlocks_ + (index >> 16) * sizeof(uintptr_t), block) || !block) return {};
    const uintptr_t entry = block + (index & 0xFFFF) * kNameStride;

    uint16_t header = 0;
    if (!Read(entry, header)) return {};
    const uint32_t length = header >> 6;
    if (length == 0 || length > 1024) return {};

    std::string result;
    if (header & 1) {
        std::wstring wide(length, L'\0');
        if (!memory::SafeRead(reinterpret_cast<const void*>(entry + 2), wide.data(), length * 2)) return {};
        int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(length), nullptr, 0, nullptr, nullptr);
        result.resize(static_cast<size_t>(n));
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(length), result.data(), n, nullptr, nullptr);
    } else {
        result.resize(length);
        if (!memory::SafeRead(reinterpret_cast<const void*>(entry + 2), result.data(), length)) return {};
    }
    if (name.number > 0) result += "_" + std::to_string(name.number - 1);
    return result;
}

// --- Objects ----------------------------------------------------------------

bool Reflection::FindObjectArray(std::string& error) {
    auto data = module::Section(GetModuleHandleW(nullptr), ".data");
    if (!data) {
        error = "no .data section";
        return false;
    }

    // FChunkedFixedUObjectArray:
    //   FUObjectItem** Objects; FUObjectItem* PreAllocatedObjects;
    //   int32 MaxElements; int32 NumElements; int32 MaxChunks; int32 NumChunks;
    ReadableCache cache;
    ForEachQword(*data, [&](uintptr_t at) {
        auto* p = reinterpret_cast<const uint8_t*>(at);
        if (at + 32 > reinterpret_cast<uintptr_t>(data->data()) + data->size()) return true;
        uintptr_t objects;
        int32_t maxElements, numElements, maxChunks, numChunks;
        std::memcpy(&objects, p, 8);
        std::memcpy(&maxElements, p + 16, 4);
        std::memcpy(&numElements, p + 20, 4);
        std::memcpy(&maxChunks, p + 24, 4);
        std::memcpy(&numChunks, p + 28, 4);
        if (!PlausiblePointer(objects)) return false;
        if (numElements <= 0 || maxElements < numElements || maxElements > 0x4000000) return false;
        if (maxChunks != (maxElements + kChunkSize - 1) / kChunkSize) return false;
        if (numChunks != (numElements + kChunkSize - 1) / kChunkSize) return false;
        if (!cache.Check(objects, static_cast<size_t>(numChunks) * 8)) return false;

        // Validate: item i's object reports InternalIndex == i.
        uintptr_t chunk0 = 0;
        if (!Read(objects, chunk0) || !PlausiblePointer(chunk0)) return false;
        for (int32_t i = 0; i < 3 && i < numElements; ++i) {
            uintptr_t object = 0;
            int32_t internalIndex = -1;
            if (!Read(chunk0 + i * kObjectItemSize, object) || !PlausiblePointer(object)) return false;
            if (!Read(object + layout_.objectIndex, internalIndex) || internalIndex != i) return false;
        }
        objectArray_ = at;
        return true;
    });

    if (!objectArray_) {
        error = "GUObjectArray not found";
        return false;
    }
    return true;
}

int32_t Reflection::ObjectCount() const {
    int32_t n = 0;
    Read(objectArray_ + 20, n);
    return n;
}

uintptr_t Reflection::ObjectAt(int32_t index) const {
    if (index < 0 || index >= ObjectCount()) return 0;
    uintptr_t objects = 0, chunk = 0, object = 0;
    if (!Read(objectArray_, objects)) return 0;
    if (!Read(objects + (index / kChunkSize) * sizeof(uintptr_t), chunk) || !chunk) return 0;
    if (!Read(chunk + (index % kChunkSize) * kObjectItemSize, object)) return 0;
    return object;
}

std::string Reflection::ObjectName(uintptr_t object) const {
    FName name;
    if (!object || !Read(object + layout_.objectName, name)) return {};
    return NameToString(name);
}

uintptr_t Reflection::ObjectClass(uintptr_t object) const {
    uintptr_t cls = 0;
    if (object) Read(object + layout_.objectClass, cls);
    return cls;
}

uintptr_t Reflection::ObjectOuter(uintptr_t object) const {
    uintptr_t outer = 0;
    if (object) Read(object + layout_.objectOuter, outer);
    return outer;
}

std::string Reflection::PathName(uintptr_t object) const {
    std::string path = ObjectName(object);
    int depth = 0;
    for (uintptr_t outer = ObjectOuter(object); outer && depth < 32; outer = ObjectOuter(outer), ++depth) {
        path = ObjectName(outer) + "." + path;
    }
    return path;
}

std::string Reflection::FullName(uintptr_t object) const {
    return ClassName(object) + " " + PathName(object);
}

uintptr_t Reflection::SuperStruct(uintptr_t ustruct) const {
    uintptr_t super = 0;
    if (ustruct && layout_.structSuper) Read(ustruct + layout_.structSuper, super);
    return super;
}

bool Reflection::IsA(uintptr_t object, uintptr_t ofClass) const {
    int depth = 0;
    for (uintptr_t c = ObjectClass(object); c && depth < 64; c = SuperStruct(c), ++depth) {
        if (c == ofClass) return true;
    }
    return false;
}

void Reflection::ForEachObject(const std::function<void(int32_t, uintptr_t)>& fn) const {
    const int32_t count = ObjectCount();
    for (int32_t i = 0; i < count; ++i) {
        if (uintptr_t object = ObjectAt(i)) fn(i, object);
    }
}

uintptr_t Reflection::FindObject(const std::string& fullName) const {
    // Cheap pre-filter on the short name before building full paths.
    const auto dot = fullName.find_last_of('.');
    const std::string shortName = dot == std::string::npos ? fullName : fullName.substr(dot + 1);
    const int32_t count = ObjectCount();
    for (int32_t i = 0; i < count; ++i) {
        uintptr_t object = ObjectAt(i);
        if (object && ObjectName(object) == shortName && FullName(object) == fullName) return object;
    }
    return 0;
}

// --- Struct / function layout discovery -----------------------------------

bool Reflection::DiscoverStructLayout(std::string& error) {
    const uintptr_t structClass = FindObject("Class /Script/CoreUObject.Struct");
    const uintptr_t functionClass = FindObject("Class /Script/CoreUObject.Function");
    const uintptr_t actorClass = FindObject("Class /Script/Engine.Actor");
    if (!structClass || !functionClass || !actorClass) {
        error = "core classes not found yet";
        return false;
    }

    // UClass'Function'->SuperStruct == UClass'Struct'
    for (uint32_t off = 0x28; off <= 0x80; off += 8) {
        uintptr_t v = 0;
        if (Read(functionClass + off, v) && v == structClass) {
            layout_.structSuper = off;
            break;
        }
    }
    if (!layout_.structSuper) {
        error = "UStruct::SuperStruct offset not found";
        return false;
    }

    // Children: first UField of AActor is a UFunction.
    for (uint32_t off = layout_.structSuper + 8; off <= layout_.structSuper + 0x30; off += 8) {
        uintptr_t v = 0;
        if (Read(actorClass + off, v) && PlausiblePointer(v) && ObjectClass(v) == functionClass) {
            layout_.structChildren = off;
            break;
        }
    }
    // ChildProperties: an FField whose FFieldClass name ends in "Property".
    for (uint32_t off = layout_.structChildren + 8; layout_.structChildren && off <= layout_.structChildren + 0x20; off += 8) {
        uintptr_t field = 0, fieldClass = 0;
        FName className;
        if (Read(actorClass + off, field) && PlausiblePointer(field) &&
            Read(field + layout_.ffieldClass, fieldClass) && PlausiblePointer(fieldClass) &&
            Read(fieldClass, className)) {
            std::string n = NameToString(className);
            if (n.size() > 8 && n.compare(n.size() - 8, 8, "Property") == 0) {
                layout_.structChildProperties = off;
                layout_.structPropertiesSize = off + 8;
                break;
            }
        }
    }
    if (!layout_.structChildren || !layout_.structChildProperties) {
        error = "UStruct Children/ChildProperties offsets not found";
        return false;
    }
    return true;
}

bool Reflection::DiscoverFunctionLayout(std::string& error) {
    // A native function with exactly one 12-byte parameter (FVector return).
    const uintptr_t fn = FindObject("Function /Script/Engine.Actor.K2_GetActorLocation");
    if (!fn) {
        error = "AActor::K2_GetActorLocation not found";
        return false;
    }
    for (uint32_t off = layout_.structPropertiesSize + 4; off < 0x100; off += 4) {
        uint32_t flags = 0;
        uint8_t numParms = 0;
        uint16_t parmsSize = 0;
        if (Read(fn + off, flags) && (flags & kFuncNative) && Read(fn + off + 4, numParms) &&
            numParms == 1 && Read(fn + off + 6, parmsSize) && parmsSize == 12) {
            layout_.functionFlags = off;
            layout_.functionParmsSize = off + 6;
            break;
        }
    }
    if (!layout_.functionFlags) {
        error = "UFunction flags/ParmsSize offsets not found";
        return false;
    }
    // UFunction::Func — the native thunk, a pointer into .text.
    for (uint32_t off = (layout_.functionFlags + 8 + 7) & ~7u; off < 0x120; off += 8) {
        uintptr_t v = 0;
        if (Read(fn + off, v) && v >= textBegin_ && v < textEnd_) {
            layout_.functionNative = off;
            break;
        }
    }
    if (!layout_.functionNative) {
        error = "UFunction::Func offset not found";
        return false;
    }
    return true;
}

bool Reflection::DiscoverProcessEvent(std::string& /*error*/) {
    // UE4.25–4.27 shipping builds place UObject::ProcessEvent at vtable index
    // 68. Accept it if the slot points into .text; the bridge verifies it
    // behaviourally before relying on it.
    constexpr uint32_t kCandidate = 68;
    uintptr_t object = ObjectAt(0), vtable = 0, slot = 0;
    if (Read(object, vtable) && Read(vtable + kCandidate * 8, slot) && slot >= textBegin_ &&
        slot < textEnd_) {
        layout_.processEventIndex = kCandidate;
    }
    return true;
}

bool Reflection::Initialize(std::string& error) {
    if (ready_) return true;
    if (auto text = module::Section(GetModuleHandleW(nullptr), ".text")) {
        textBegin_ = reinterpret_cast<uintptr_t>(text->data());
        textEnd_ = textBegin_ + text->size();
    }
    if (!nameBlocks_ && !FindNamePool(error)) return false;
    if (!objectArray_ && !FindObjectArray(error)) return false;
    if (!layout_.structChildProperties && !DiscoverStructLayout(error)) return false;
    if (!layout_.functionNative && !DiscoverFunctionLayout(error)) return false;
    DiscoverProcessEvent(error);
    ready_ = true;
    return true;
}

// --- Struct contents ---------------------------------------------------------

std::vector<uintptr_t> Reflection::Functions(uintptr_t ustruct) const {
    std::vector<uintptr_t> result;
    uintptr_t child = 0;
    if (!Read(ustruct + layout_.structChildren, child)) return result;
    const std::string kFunction = "Function";
    for (int guard = 0; child && guard < 4096; ++guard) {
        if (ClassName(child) == kFunction) result.push_back(child);
        if (!Read(child + layout_.fieldNext, child)) break;
    }
    return result;
}

std::vector<PropertyInfo> Reflection::Properties(uintptr_t ustruct) const {
    std::vector<PropertyInfo> result;
    uintptr_t field = 0;
    if (!Read(ustruct + layout_.structChildProperties, field)) return result;
    for (int guard = 0; field && guard < 4096; ++guard) {
        PropertyInfo info;
        FName name, typeName;
        uintptr_t fieldClass = 0;
        Read(field + layout_.ffieldName, name);
        if (Read(field + layout_.ffieldClass, fieldClass)) Read(fieldClass, typeName);
        info.name = NameToString(name);
        info.type = NameToString(typeName);
        Read(field + layout_.propertyOffset, info.offset);
        Read(field + layout_.propertyElementSize, info.elementSize);
        Read(field + layout_.propertyArrayDim, info.arrayDim);
        Read(field + layout_.propertyFlags, info.flags);
        result.push_back(std::move(info));
        if (!Read(field + layout_.ffieldNext, field)) break;
    }
    return result;
}

int32_t Reflection::PropertiesSize(uintptr_t ustruct) const {
    int32_t size = 0;
    Read(ustruct + layout_.structPropertiesSize, size);
    return size;
}

// FImplementedInterface { UClass* Class; int32 PointerOffset; bool bK2; } (16 bytes)
namespace {
constexpr uint32_t kImplementedInterfaceStride = 16;

bool LooksLikeInterfaceArray(const Reflection& r, uintptr_t cls, uint32_t offset,
                             uintptr_t classClass) {
    uintptr_t data = 0;
    int32_t num = 0, max = 0;
    if (!memory::Read(cls + offset, data) || !memory::Read(cls + offset + 8, num) ||
        !memory::Read(cls + offset + 12, max)) {
        return false;
    }
    if (num < 1 || num > 64 || max < num || max > 4096) return false;
    if (!memory::IsReadable(data, static_cast<size_t>(num) * kImplementedInterfaceStride)) return false;
    // Validate the first few elements: Class is a UClass, offset/flag sane.
    for (int32_t i = 0; i < num && i < 3; ++i) {
        uintptr_t elem = data + static_cast<uintptr_t>(i) * kImplementedInterfaceStride;
        uintptr_t ifaceClass = 0;
        int32_t pointerOffset = 0;
        uint8_t k2 = 0;
        if (!memory::Read(elem, ifaceClass) || !memory::Read(elem + 8, pointerOffset) ||
            !memory::Read(elem + 12, k2)) {
            return false;
        }
        if (r.ObjectClass(ifaceClass) != classClass) return false;
        if (pointerOffset < 0 || pointerOffset > 0x8000 || k2 > 1) return false;
    }
    return true;
}
}  // namespace

bool Reflection::DiscoverInterfaceOffset() {
    if (layout_.classInterfaces) return true;
    const uintptr_t classClass = FindObject("Class /Script/CoreUObject.Class");
    if (!classClass) return false;

    // Collect a sample of UClass objects.
    std::vector<uintptr_t> classes;
    const int32_t count = ObjectCount();
    for (int32_t i = 0; i < count && classes.size() < 3000; ++i) {
        uintptr_t object = ObjectAt(i);
        if (object && ObjectClass(object) == classClass) classes.push_back(object);
    }
    if (classes.size() < 50) return false;

    // Vote: the offset where the most classes expose a valid interface array.
    const uint32_t begin = (layout_.structPropertiesSize + 8 + 7) & ~7u;
    uint32_t bestOffset = 0;
    int bestVotes = 0;
    for (uint32_t off = begin; off <= begin + 0x400; off += 8) {
        int votes = 0;
        for (uintptr_t cls : classes) {
            if (LooksLikeInterfaceArray(*this, cls, off, classClass)) ++votes;
        }
        if (votes > bestVotes) {
            bestVotes = votes;
            bestOffset = off;
        }
    }
    // Require a clear signal (many classes implement interfaces).
    if (bestVotes < 20) return false;
    layout_.classInterfaces = bestOffset;
    return true;
}

std::vector<uintptr_t> Reflection::Implementers(uintptr_t interfaceClass) const {
    std::vector<uintptr_t> result;
    if (!layout_.classInterfaces || !interfaceClass) return result;
    const uintptr_t classClass = FindObject("Class /Script/CoreUObject.Class");
    ForEachObject([&](int32_t, uintptr_t object) {
        if (ObjectClass(object) != classClass) return;
        uintptr_t data = 0;
        int32_t num = 0;
        if (!memory::Read(object + layout_.classInterfaces, data) ||
            !memory::Read(object + layout_.classInterfaces + 8, num) || num < 1 || num > 64) {
            return;
        }
        for (int32_t i = 0; i < num; ++i) {
            uintptr_t iface = 0;
            if (memory::Read(data + static_cast<uintptr_t>(i) * kImplementedInterfaceStride, iface) &&
                iface == interfaceClass) {
                result.push_back(object);
                return;
            }
        }
    });
    return result;
}

uintptr_t Reflection::FindLiveInstance(uintptr_t ofClass) const {
    if (!ofClass) return 0;
    uintptr_t found = 0;
    ForEachObject([&](int32_t, uintptr_t object) {
        if (found) return;
        if (ObjectName(object).rfind("Default__", 0) == 0) return;
        if (IsA(object, ofClass)) found = object;
    });
    return found;
}

int32_t Reflection::ParmsSize(uintptr_t ufunction) const {
    uint16_t size = 0;
    memory::Read(ufunction + layout_.functionParmsSize, size);
    return size;
}

int32_t Reflection::PropertyOffset(uintptr_t ustruct, const std::string& name) const {
    for (const auto& p : Properties(ustruct)) {
        if (p.name == name) return p.offset;
    }
    return -1;
}

uint32_t Reflection::FunctionFlags(uintptr_t ufunction) const {
    uint32_t flags = 0;
    Read(ufunction + layout_.functionFlags, flags);
    return flags;
}

uintptr_t Reflection::FunctionNative(uintptr_t ufunction) const {
    uintptr_t fn = 0;
    Read(ufunction + layout_.functionNative, fn);
    return fn;
}

// --- Dump --------------------------------------------------------------------

bool Reflection::Dump(const std::wstring& path, const std::function<bool(const std::string&)>& filter,
                      std::string& error) const {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) {
        error = "cannot open dump file";
        return false;
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    auto rva = [&](uintptr_t p) { return p ? Hex(p - base) : std::string("0"); };

    fprintf(f, "# Overseer reflection dump — generated locally; do not commit (see docs/legal-and-fair-play.md)\n");
    fprintf(f, "names.blocks        = base+%s\n", rva(nameBlocks_).c_str());
    fprintf(f, "objects.array       = base+%s  count=%d\n", rva(objectArray_).c_str(), ObjectCount());
    fprintf(f, "ustruct.super       = 0x%x\n", layout_.structSuper);
    fprintf(f, "ustruct.children    = 0x%x\n", layout_.structChildren);
    fprintf(f, "ustruct.childprops  = 0x%x\n", layout_.structChildProperties);
    fprintf(f, "ustruct.propsize    = 0x%x\n", layout_.structPropertiesSize);
    fprintf(f, "ufunction.flags     = 0x%x\n", layout_.functionFlags);
    fprintf(f, "ufunction.parmssize = 0x%x\n", layout_.functionParmsSize);
    fprintf(f, "ufunction.func      = 0x%x\n", layout_.functionNative);
    fprintf(f, "uobject.processevent vtable index = %u\n\n", layout_.processEventIndex);

    std::map<std::string, uintptr_t> matches;
    ForEachObject([&](int32_t, uintptr_t object) {
        const std::string cls = ClassName(object);
        if (cls != "Class" && cls != "Function" && cls != "ScriptStruct" && cls != "Enum") return;
        std::string full = FullName(object);
        if (filter(full)) matches.emplace(std::move(full), object);
    });

    for (const auto& [full, object] : matches) {
        const std::string cls = ClassName(object);
        if (cls == "Function") {
            const uint32_t flags = FunctionFlags(object);
            uint16_t parmsSize = 0;
            Read(object + layout_.functionParmsSize, parmsSize);
            fprintf(f, "%s\n  flags=0x%08x parms=%u native=base+%s\n", full.c_str(), flags,
                    parmsSize, rva(FunctionNative(object)).c_str());
            for (const auto& p : Properties(object)) {
                fprintf(f, "    +0x%03x %-16s %-40s size=%d flags=0x%llx\n", p.offset, p.type.c_str(),
                        p.name.c_str(), p.elementSize, static_cast<unsigned long long>(p.flags));
            }
        } else if (cls == "Class" || cls == "ScriptStruct") {
            const uintptr_t super = SuperStruct(object);
            fprintf(f, "%s : %s  size=0x%x\n", full.c_str(), super ? PathName(super).c_str() : "-",
                    PropertiesSize(object));
            for (const auto& p : Properties(object)) {
                fprintf(f, "    +0x%03x %-16s %s\n", p.offset, p.type.c_str(), p.name.c_str());
            }
            for (uintptr_t fn : Functions(object)) {
                fprintf(f, "    fn %s\n", ObjectName(fn).c_str());
            }
        } else {
            fprintf(f, "%s\n", full.c_str());
        }
    }
    fclose(f);
    log::Info("reflection dump: " + std::to_string(matches.size()) + " objects written");
    return true;
}

}  // namespace overseer::ue4
