#include "mods/captured_candidate.hpp"
#include "mods/mod_definition_parser.hpp"
#include "scripting/source_project.hpp"
#include "scripting/luau_compiler.hpp"
#include "../../third_party/tomlplusplus/toml.hpp"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <set>
#include <stdexcept>

namespace sc13::mods {
namespace {
constexpr std::size_t MiB=1024U*1024U;
/** @summary Mark native bounded reason codes separately from untrusted parser/compiler exceptions. */
class CaptureFailure final : public std::runtime_error {
public:
    /** @summary Retain a native static reason code. @param reason Host-authored bounded code. */
    explicit CaptureFailure(const char* reason):std::runtime_error(reason) {}
};
/** @summary Reject without exposing payloads or filesystem paths. @param reason Bounded native reason code. */
[[noreturn]] void Reject(const char* reason) { throw CaptureFailure(reason); }
/** @summary Retain one non-following plain file/directory lease denying writes and deletion. */
struct Lease final {
    HANDLE handle{INVALID_HANDLE_VALUE};
    BY_HANDLE_FILE_INFORMATION info{};
    /** @summary Open and identify a plain disk object. @param path Native path. @param directory Expected kind. */
    Lease(const std::filesystem::path& path,bool directory) {
        handle=CreateFileW(scripting::NativePath(path).c_str(),directory ? FILE_READ_ATTRIBUTES : GENERIC_READ,
            FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT |
            (directory ? FILE_FLAG_BACKUP_SEMANTICS : FILE_FLAG_SEQUENTIAL_SCAN),nullptr);
        if(handle==INVALID_HANDLE_VALUE) Reject("candidate.lease");
        if(!GetFileInformationByHandle(handle,&info) || GetFileType(handle)!=FILE_TYPE_DISK ||
            (info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT) ||
            bool(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=directory || (!directory && info.nNumberOfLinks!=1)) {
            CloseHandle(handle); handle=INVALID_HANDLE_VALUE; Reject("candidate.file-kind");
        }
    }
    /** @summary Release the original native lease. */
    ~Lease() { if(handle!=INVALID_HANDLE_VALUE) CloseHandle(handle); }
    Lease(const Lease&)=delete;
    Lease& operator=(const Lease&)=delete;
};
/** @summary Compare exact file identity and relevant enumeration observations.
 * @param a Original identity. @param b Current identity. @return Exact selected-object match.
 */
bool Same(const BY_HANDLE_FILE_INFORMATION& a,const BY_HANDLE_FILE_INFORMATION& b) {
    return a.dwVolumeSerialNumber==b.dwVolumeSerialNumber && a.nFileIndexHigh==b.nFileIndexHigh &&
        a.nFileIndexLow==b.nFileIndexLow && a.nFileSizeHigh==b.nFileSizeHigh && a.nFileSizeLow==b.nFileSizeLow &&
        a.ftLastWriteTime.dwHighDateTime==b.ftLastWriteTime.dwHighDateTime &&
        a.ftLastWriteTime.dwLowDateTime==b.ftLastWriteTime.dwLowDateTime;
}
/** @summary Validate one portable lowercase path without device or traversal aliases.
 * @param name Relative UTF8/ASCII name. @return Number of validated components.
 */
std::size_t Name(std::string_view name) {
    if(name.empty() || name.size()>240 || name.front()=='/' || name.back()=='/') Reject("candidate.path");
    std::size_t count{},begin{};
    while(begin<name.size()) {
        const auto end=name.find('/',begin);
        const auto part=name.substr(begin,end==name.npos ? name.size()-begin : end-begin);
        if(part.empty() || part=="." || part==".." || part.back()=='.' || ++count>16) Reject("candidate.path");
        for(unsigned char c:part) if(!(c>='a'&&c<='z') && !(c>='0'&&c<='9') && c!='.' && c!='-' && c!='_') Reject("candidate.path");
        const auto stem=part.substr(0,part.find('.'));
        if(stem=="con" || stem=="prn" || stem=="aux" || stem=="nul" ||
            (stem.size()==4 && (stem.starts_with("com") || stem.starts_with("lpt")) && stem[3]>='0' && stem[3]<='9')) Reject("candidate.path");
        if(end==name.npos) break;
        begin=end+1;
    }
    return count;
}
/** @summary Convert a relative filesystem spelling without normalizing aliases.
 * @param path Relative path. @return Exact portable validated name.
 */
std::string Relative(const std::filesystem::path& path) {
    const auto raw=path.generic_u8string();
    std::string result(reinterpret_cast<const char*>(raw.data()),raw.size()); Name(result); return result;
}
using Inventory=std::map<std::string,BY_HANDLE_FILE_INFORMATION>;
/** @summary Reject fixed-input case aliases using the actual directory entry spelling.
 * @param root Retained native root. @param name Exact selected child spelling.
 */
void Spelling(const std::filesystem::path& root,const wchar_t* name) {
    WIN32_FIND_DATAW info{};
    HANDLE search=FindFirstFileW(scripting::NativePath(root/name).c_str(),&info);
    if(search==INVALID_HANDLE_VALUE) Reject("candidate.lease");
    FindClose(search);
    if(std::wstring_view(info.cFileName)!=name) Reject("candidate.path");
}
/** @summary Enumerate only bounded selected inputs and retain directory path identities.
 * @param root Source root. @param directories Retained directory leases. @return Exact selected file observations.
 */
Inventory Enumerate(const std::filesystem::path& root,std::vector<std::unique_ptr<Lease>>& directories) {
    Inventory result;
    /** @summary Add one plain selected file before allocating its bytes. @param name Portable name. */
    const auto add=[&](const std::string& name) {
        Name(name); if(result.size()>=257) Reject("candidate.file-count");
        Lease file(root/name,false);
        if(file.info.nFileSizeHigh || file.info.nFileSizeLow>MiB) Reject("candidate.file-size");
        if(!result.emplace(name,file.info).second) Reject("candidate.duplicate");
    };
    Spelling(root,L"mod.toml");add("mod.toml");
    if(std::filesystem::exists(root/"project.toml") || std::filesystem::exists(root/"mod.toon")) Reject("candidate.ambiguous-manifest");
    if(!std::filesystem::exists(root/"scripts")) return result;
    Spelling(root,L"scripts");
    std::vector<std::filesystem::path> pending{root/"scripts"};
    while(!pending.empty()) {
        auto directory=std::move(pending.back());pending.pop_back();
        Relative(directory.lexically_relative(root));
        if(directories.size()>=64) Reject("candidate.directory-count");
        directories.push_back(std::make_unique<Lease>(directory,true));
        std::size_t entries{};
        for(const auto& item:std::filesystem::directory_iterator(directory)) {
            if(++entries>321 || pending.size()>64) Reject("candidate.inventory-count");
            const auto name=Relative(item.path().lexically_relative(root));
            const auto attributes=GetFileAttributesW(scripting::NativePath(item.path()).c_str());
            if(attributes==INVALID_FILE_ATTRIBUTES || (attributes&FILE_ATTRIBUTE_REPARSE_POINT)) Reject("candidate.file-kind");
            if(attributes&FILE_ATTRIBUTE_DIRECTORY) pending.push_back(item.path());
            else { if(!name.ends_with(".luau")) Reject("candidate.script-kind"); add(name); }
        }
    }
    return result;
}
/** @summary Reject unknown typed fields on the isolated route only.
 * @param node Expected table. @param allowed Exact recognized names.
 */
void Fields(const toml::node* node,std::initializer_list<std::string_view> allowed) {
    if(!node || !node->is_table()) Reject("candidate.manifest-shape");
    for(const auto& [key,value]:*node->as_table()) {
        static_cast<void>(value);
        if(std::find(allowed.begin(),allowed.end(),key.str())==allowed.end()) Reject("candidate.manifest-field");
    }
}
/** @summary Validate closed-route shape before shared legacy semantics parse captured bytes.
 * @param document Parsed captured authoring document.
 */
void Shape(const toml::table& document) {
    Fields(&document,{"schemaVersion","format","kind","metadata","scripts","patches","symbols"});
    Fields(document.get("metadata"),{"id","name","version","author","description"});
    for(const auto& [key,value]:*document.get("metadata")->as_table()) {
        static_cast<void>(key); if(!value.is_string()) Reject("candidate.metadata-shape");
    }
    if(document.contains("kind") && document["kind"].value<std::string>()!="gameplay") Reject("candidate.kind");
    if(document.contains("scripts")) Fields(document.get("scripts"),{"language","entry","memoryMiB","budgetMs"});
    if(const auto* symbols=document.get("symbols")) {
        Fields(symbols,{"resources","properties"});
        for(const auto& [kind,node]:*symbols->as_table()) {
            const auto* entries=node.as_array();
            if(!entries || entries->size()>4096) Reject("candidate.symbol-count");
            for(const auto& entry:*entries) {
                if(kind=="resources") Fields(&entry,{"name","source","type","group","instance"});
                else Fields(&entry,{"name","source","id","description","unit","origin","type"});
            }
        }
    }
    if(const auto* patches=document.get("patches")) {
        const auto* entries=patches->as_array(); if(!entries || entries->size()>4096) Reject("candidate.patch-count");
        std::size_t count{};
        for(const auto& patch:*entries) {
            Fields(&patch,{"target","properties"});
            Fields(patch.as_table()->get("target"),{"name","type","group","instance"});
            const auto* properties=patch.as_table()->get_as<toml::array>("properties");
            if(!properties || properties->size()>4096-count) Reject("candidate.property-count");
            count+=properties->size();
            for(const auto& property:*properties) Fields(&property,{"name","id","type","operation","value"});
        }
    }
}
/** @summary Validate every part of the copied Base inventory without authenticating it.
 * @param base Caller-owned pin copy.
 */
void Base(const scripting::BasePackagePin& base) {
    if(base.version.empty() || base.version.size()>128 || base.files.size()<2 || base.files.size()>64 ||
        !base.files.contains("mod.toml") || !base.files.contains("scripts/init.luau")) Reject("candidate.base-pin");
    for(const auto& [name,hash]:base.files) {
        Name(name);
        if((name!="mod.toml" && (!name.starts_with("scripts/") || !name.ends_with(".luau"))) || hash.size()!=64 ||
            !std::all_of(hash.begin(),hash.end(),[](char c){return(c>='0'&&c<='9')||(c>='A'&&c<='F');})) Reject("candidate.base-pin");
    }
}
/** @summary Append unambiguous length-framed bytes to bounded deterministic digest input.
 * @param target Hash input. @param value Exact next field.
 */
void Frame(std::string& target,std::string_view value) {
    const auto length=static_cast<std::uint64_t>(value.size());
    for(unsigned i=0;i<8;++i) target.push_back(static_cast<char>((length>>(i*8))&255));
    target.append(value);
}
}
std::shared_ptr<const InstallationBinding> InstallationBinding::Create() {
    return std::shared_ptr<const InstallationBinding>(new InstallationBinding);
}
std::shared_ptr<const CapturedCandidate> CapturedCandidate::Capture(const std::filesystem::path& directory,
    std::shared_ptr<const InstallationBinding> installation,scripting::BasePackagePin base) {
    return CaptureImpl(directory,std::move(installation),std::move(base)
#ifdef SIMODER_RUNTIME_TEST_AUTHORITY
        ,{}
#endif
    );
}
std::shared_ptr<const CapturedCandidate> CapturedCandidate::CaptureImpl(const std::filesystem::path& directory,
    std::shared_ptr<const InstallationBinding> installation,scripting::BasePackagePin base
#ifdef SIMODER_RUNTIME_TEST_AUTHORITY
    ,const std::function<void(CandidateCapturePhase)>& observer
#endif
) {
    try {
        if(!installation) Reject("candidate.installation"); Base(base);
        const auto root=std::filesystem::absolute(directory).lexically_normal();
        if(root.root_name().native().size()!=2 || root.root_name().native()[1]!=L':') Reject("candidate.root");
        std::vector<std::unique_ptr<Lease>> ancestors,directories;
        auto path=root.root_path(); ancestors.push_back(std::make_unique<Lease>(path,true));
        for(const auto& component:root.relative_path()) {
            if(ancestors.size()>=64) Reject("candidate.ancestor-count");
            path/=component; ancestors.push_back(std::make_unique<Lease>(path,true));
        }
        const auto expected=Enumerate(root,directories);
#ifdef SIMODER_RUNTIME_TEST_AUTHORITY
        if(observer) observer(CandidateCapturePhase::AfterInventory);
#endif
        std::vector<std::unique_ptr<Lease>> files;
        std::size_t sourceTotal{},total{};
        for(const auto& [name,info]:expected) {
            auto file=std::make_unique<Lease>(root/name,false);
            if(!Same(info,file->info)) Reject("candidate.input-changed");
            const auto size=static_cast<std::size_t>(file->info.nFileSizeLow);
            if(file->info.nFileSizeHigh || size>MiB || size>9*MiB-total) Reject("candidate.byte-limit");
            total+=size;
            if(name!="mod.toml") { if(size>8*MiB-sourceTotal) Reject("candidate.source-limit"); sourceTotal+=size; }
            files.push_back(std::move(file));
        }
#ifdef SIMODER_RUNTIME_TEST_AUTHORITY
        if(observer) observer(CandidateCapturePhase::AfterLeases);
#endif
        auto result=std::shared_ptr<CapturedCandidate>(new CapturedCandidate);
        std::size_t index{};
        for(const auto& [name,info]:expected) {
            std::string bytes(info.nFileSizeLow,'\0'); DWORD read{};
            if(!bytes.empty() && (!ReadFile(files[index]->handle,bytes.data(),static_cast<DWORD>(bytes.size()),&read,nullptr) || read!=bytes.size())) Reject("candidate.read");
            ++index; result->inventory_.emplace(name,std::move(bytes));
        }
#ifdef SIMODER_RUNTIME_TEST_AUTHORITY
        if(observer) observer(CandidateCapturePhase::AfterRead);
#endif
        std::vector<std::unique_ptr<Lease>> finalDirectories;
        const auto actual=Enumerate(root,finalDirectories);
        if(actual.size()!=expected.size()) Reject("candidate.inventory-changed");
        for(const auto& [name,info]:expected) {
            const auto found=actual.find(name);
            if(found==actual.end() || !Same(info,found->second)) Reject("candidate.input-changed");
        }
        const auto& manifest=result->inventory_.at("mod.toml");
        Shape(toml::parse(manifest));
        const auto projection=scripting::InspectSourceManifest(manifest);
        if(projection.metadata.id=="simoder.base" || (!projection.language.empty() && projection.language!="luau")) Reject("candidate.language");
        result->definition_.manifest=projection.metadata;
        result->definition_.directory=root;
        std::string error;
        if(!ParseAuthoringPatches(manifest,result->definition_.patches,error)) Reject("candidate.patches");
        std::shared_ptr<scripting::ScriptDefinition> script;
        if(!projection.language.empty()) {
            Name(projection.entry);
            if(!result->inventory_.contains(projection.entry)) Reject("candidate.entry");
            script=std::make_shared<scripting::ScriptDefinition>();
            script->entry=projection.entry;script->language="luau";
            script->memoryBytes=projection.memoryBytes;script->budgetMs=projection.budgetMs;
            if(!ParseScriptSymbols(manifest,*script,error)) Reject("candidate.symbols");
            std::size_t compiled{};
            for(const auto& [name,bytes]:result->inventory_) {
                if(name=="mod.toml") continue;
                auto code=scripting::CompileLuauSource(bytes);
                const auto copies=name==script->entry ? 2U : 1U;
                if(code.size()>(16*MiB-compiled)/copies) Reject("candidate.output-limit");
                compiled+=code.size()*copies;
                if(name==script->entry) script->bytecode=code;
                script->modules.emplace(name,std::move(code));
            }
        } else if(result->inventory_.size()!=1 || result->definition_.patches.empty()) Reject("candidate.empty");
        std::string digestInput;
        Frame(digestInput,"simoder-captured-candidate-v1");
        Frame(digestInput,scripting::LuauCompilationProfile());
        Frame(digestInput,base.version); Frame(digestInput,std::to_string(base.files.size()));
        for(const auto& [name,hash]:base.files) { Frame(digestInput,name);Frame(digestInput,hash); }
        Frame(digestInput,std::to_string(result->inventory_.size()));
        for(const auto& [name,bytes]:result->inventory_) { Frame(digestInput,name);Frame(digestInput,bytes); }
        Frame(digestInput,script ? "luau" : "declarative");
        if(script) {
            Frame(digestInput,script->entry);Frame(digestInput,script->bytecode);
            Frame(digestInput,std::to_string(script->modules.size()));
            for(const auto& [name,code]:script->modules) { Frame(digestInput,name);Frame(digestInput,code); }
        }
        result->digest_=scripting::HashText(digestInput);
        if(script) {script->revision=result->digest_;result->definition_.script=std::move(script);}
        result->base_=std::move(base);result->installation_=std::move(installation);
        return result;
    } catch(const CaptureFailure&) { throw; }
    catch(...) { Reject("candidate.invalid-input"); }
}
}
