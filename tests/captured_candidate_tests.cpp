#include "mods/captured_candidate.hpp"
#include "mods/mod_definition_parser.hpp"
#include "scripting/luau_compiler.hpp"
#include <Windows.h>
#include <winioctl.h>
#include <filesystem>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
using namespace sc13;
int checks{};
/** @summary Assert an observable isolated invariant. @param value Observed result. @param message Failure description. */
void Check(bool value,const char* message) { ++checks;if(!value) throw std::runtime_error(std::string(message)+" at check "+std::to_string(checks)); }
/** @summary Write synthetic authoring bytes. @param path Owned fixture path. @param bytes Complete content. */
void Write(const std::filesystem::path& path,std::string_view bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path,std::ios::binary|std::ios::trunc);out.write(bytes.data(),static_cast<std::streamsize>(bytes.size()));
    if(!out) throw std::runtime_error("Fixture write failed");
}
/** @summary Produce a current authoring manifest with native alias provenance. @return Complete synthetic manifest. */
std::string Manifest() {
    return "schemaVersion=2\nformat='simoder.authoring'\n[metadata]\nid='capture.fixture'\nname='Capture'\nversion='1'\n"
        "[scripts]\nlanguage='luau'\nentry='scripts/main.luau'\nmemoryMiB=8\nbudgetMs=2\n"
        "[[symbols.resources]]\nname='asset'\ntype=1\ngroup=2\ninstance=3\nsource='synthetic'\n"
        "[[symbols.properties]]\nname='cost'\nid=4\ntype='float'\nsource='synthetic'\n"
        "[[patches]]\ntarget={name='asset'}\nproperties=[{name='cost',type='float',operation='set',value=7}]\n";
}
/** @summary Produce a structurally complete test Base pin. @return Unauthenticated synthetic pin. */
scripting::BasePackagePin Pin() {
    return {"fixture-base",{{"mod.toml",std::string(64,'A')},{"scripts/init.luau",std::string(64,'B')}}};
}
/** @summary Initialize one owned package. @param root Synthetic directory. */
void Package(const std::filesystem::path& root) {
    Write(root/"mod.toml",Manifest());
    Write(root/"scripts/main.luau","local helper=require('./helper'); return {onEnable=function() simoder.log(helper) end}");
    Write(root/"scripts/helper.luau","return 'captured-ok'");
}
/** @summary Require bounded capture rejection. @param action Isolated operation. @param reason Expected code or any bounded code. */
template<class F> void Reject(F action,std::string_view reason={}) {
    try { action(); } catch(const std::exception& error) {
        const std::string_view text(error.what());
        Check(text.starts_with("candidate.") && text.size()<100,"Unbounded or payload-bearing rejection");
        if(!reason.empty() && text!=reason) throw std::runtime_error("Unexpected rejection at check "+std::to_string(checks)+": expected "+std::string(reason)+", actual "+std::string(text));
        Check(true,"Expected bounded rejection");return;
    }
    throw std::runtime_error("Expected capture rejection");
}
/** @summary Remove only this test's generated directory on exit. */
struct Fixture final {
    std::filesystem::path root=std::filesystem::current_path()/("captured-fixture-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));
    /** @summary Release bounded owned synthetic fixtures. */
    ~Fixture() { std::error_code error;std::filesystem::remove_all(root,error); }
};
/** @summary Create one synthetic directory junction without enabling privileges.
 * @param link Empty owned directory location. @param target Owned directory target.
 */
void Junction(const std::filesystem::path& link,const std::filesystem::path& target) {
    std::filesystem::create_directories(link);std::filesystem::create_directories(target);
    const auto substitute=L"\\??\\"+std::filesystem::absolute(target).native();
    const auto print=std::filesystem::absolute(target).native();
    std::vector<std::byte> data(16+(substitute.size()+print.size()+2)*sizeof(wchar_t));
    const DWORD tag=IO_REPARSE_TAG_MOUNT_POINT;
    const WORD length=static_cast<WORD>(data.size()-8),subLength=static_cast<WORD>(substitute.size()*2),
        printOffset=static_cast<WORD>(subLength+2),printLength=static_cast<WORD>(print.size()*2);
    std::memcpy(data.data(),&tag,4);std::memcpy(data.data()+4,&length,2);
    std::memcpy(data.data()+10,&subLength,2);std::memcpy(data.data()+12,&printOffset,2);std::memcpy(data.data()+14,&printLength,2);
    std::memcpy(data.data()+16,substitute.data(),subLength);std::memcpy(data.data()+16+printOffset,print.data(),printLength);
    HANDLE handle=CreateFileW(link.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    Check(handle!=INVALID_HANDLE_VALUE,"Junction handle unavailable");DWORD returned{};
    const bool created=DeviceIoControl(handle,FSCTL_SET_REPARSE_POINT,data.data(),static_cast<DWORD>(data.size()),nullptr,0,&returned,nullptr)!=FALSE;
    CloseHandle(handle);Check(created,"Junction fixture unavailable");
}
/** @summary Exercise immutable execution, deterministic identity and separate installation bindings. @param root Owned fixture root. */
void Immutable(const std::filesystem::path& root) {
    const auto a=root/"a",b=root/"b";Package(a);Package(b);
    const auto binding=mods::InstallationBinding::Create();
    auto pin=Pin();
    const auto first=mods::CapturedCandidate::Capture(a,binding,pin);
    const auto second=mods::CapturedCandidate::Capture(b,binding,pin);
    const auto reinstall=mods::CapturedCandidate::Capture(b,mods::InstallationBinding::Create(),pin);
    Check(first->ContentDigest()==second->ContentDigest(),"Path or enumeration affected digest");
    Check(first->SameInstallation(*second),"Retained binding changed installation");
    Check(!first->SameInstallation(*reinstall) && first->ContentDigest()==reinstall->ContentDigest(),"Content confused with installation");
    Check(first->Inventory().size()==3 && first->Inventory().at("mod.toml")==Manifest(),"Consumed manifest not retained");
    Check(first->Definition().patches.size()==1 && first->Definition().patches[0].properties[0].value==7,"Declarative semantics changed");
    const auto& script=*first->Definition().script;
    Check(script.memoryBytes==8*1024*1024 && script.budgetMs==2 && script.resourceAliases.contains("asset") && script.propertyAliases.contains("cost"),"Aliases or limits lost");
    Check(script.modules.at("scripts/helper.luau")==scripting::CompileLuauSource(first->Inventory().at("scripts/helper.luau")),"Output not derived from captured source");
    Check(script.bytecode==script.modules.at(script.entry) && script.revision==first->ContentDigest(),"Entry/output provenance inconsistent");
    Write(a/".simoder/cache/descriptor.toml","tampered cache");Write(a/"README.md","untrusted instructions");
    const auto ignored=mods::CapturedCandidate::Capture(a,binding,pin);
    Check(first->ContentDigest()==ignored->ContentDigest(),"Cache gained content authority");
    pin.files["scripts/other.luau"]=std::string(64,'C');
    const auto changedBase=mods::CapturedCandidate::Capture(a,binding,pin);
    Check(first->ContentDigest()!=changedBase->ContentDigest(),"Full Base inventory omitted from digest");
    pin.version="changed";
    Check(first->BasePin().version=="fixture-base" && first->BasePin().files.size()==2,"Caller mutation changed copied Base pin");
    Write(a/"scripts/helper.luau","return 'changed'");
    const auto changed=mods::CapturedCandidate::Capture(a,binding,Pin());
    Check(changed->SameInstallation(*first) && changed->ContentDigest()!=first->ContentDigest(),"Changed content retained approval equality");
    std::filesystem::remove_all(a);
    scripting::ScriptRuntime vm(first->Definition().manifest.id,script);vm.Commit();
    Check(vm.DrainLogs()==std::vector<std::string>{"captured-ok"},"VM reopened deleted source or lost module behavior");
    vm.Stop();
    Check(!scripting::LuauCompilationProfile().empty(),"Compiler interpretation missing");
    const auto declarative=root/"declarative";Package(declarative);
    auto declarativeManifest=Manifest();
    const auto begin=declarativeManifest.find("[scripts]");
    declarativeManifest.erase(begin,declarativeManifest.find("[[symbols.resources]]",begin)-begin);
    Write(declarative/"mod.toml",declarativeManifest);std::filesystem::remove_all(declarative/"scripts");
    const auto plain=mods::CapturedCandidate::Capture(declarative,binding,Pin());
    Check(!plain->Definition().script && plain->Definition().patches.size()==1 && plain->Inventory().size()==1,"Declarative-only capture changed behavior");
}
/** @summary Exercise selected input lease and path replacement boundaries. @param root Owned fixture root. */
void Races(const std::filesystem::path& root) {
    Package(root);
    const auto binding=mods::InstallationBinding::Create();
    bool attempted{};
    const auto captured=mods::CapturedCandidateTestAccess::Capture(root,binding,Pin(),[&](mods::CandidateCapturePhase phase) {
        if(phase!=mods::CandidateCapturePhase::AfterLeases)return;
        for(const auto* name:{"mod.toml","scripts/helper.luau"}) {
            HANDLE writer=CreateFileW((root/name).c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
            Check(writer==INVALID_HANDLE_VALUE && GetLastError()==ERROR_SHARING_VIOLATION,"Selected write/ABA admitted under lease");
            if(writer!=INVALID_HANDLE_VALUE)CloseHandle(writer);
            Check(!MoveFileExW((root/name).c_str(),(root/"renamed.luau").c_str(),0),"Selected rename admitted under lease");
            Check(!DeleteFileW((root/name).c_str()),"Selected delete admitted under lease");
        }
        attempted=true;
    });
    Check(attempted && captured->Inventory().at("scripts/helper.luau")=="return 'captured-ok'","Leased bytes changed");
    Reject([&]{static_cast<void>(mods::CapturedCandidateTestAccess::Capture(root,binding,Pin(),[&](mods::CandidateCapturePhase phase) {
        if(phase==mods::CandidateCapturePhase::AfterInventory) {
            WIN32_FILE_ATTRIBUTE_DATA original{};
            Check(GetFileAttributesExW((root/"scripts/helper.luau").c_str(),GetFileExInfoStandard,&original)!=FALSE,"Original identity observation failed");
            std::filesystem::rename(root/"scripts/helper.luau",root/"scripts/old.luau");
            Write(root/"scripts/helper.luau","return 'captured-ok'");
            HANDLE replacement=CreateFileW((root/"scripts/helper.luau").c_str(),FILE_WRITE_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
            Check(replacement!=INVALID_HANDLE_VALUE && SetFileTime(replacement,nullptr,nullptr,&original.ftLastWriteTime),"Same size/time replacement fixture failed");
            CloseHandle(replacement);
        }
    }));},"candidate.input-changed");
    std::filesystem::remove(root/"scripts/old.luau");Package(root);
    Reject([&]{static_cast<void>(mods::CapturedCandidateTestAccess::Capture(root,binding,Pin(),[&](mods::CandidateCapturePhase phase) {
        if(phase==mods::CandidateCapturePhase::AfterRead)Write(root/"scripts/new.luau","return 1");
    }));},"candidate.inventory-changed");
    std::filesystem::remove(root/"scripts/new.luau");
    HANDLE writer=CreateFileW((root/"mod.toml").c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
    Check(writer!=INVALID_HANDLE_VALUE,"Writable fixture handle unavailable");
    Reject([&]{static_cast<void>(mods::CapturedCandidate::Capture(root,binding,Pin()));},"candidate.lease");CloseHandle(writer);
}
/** @summary Retain a preexisting writable mapping independently from its original file handle. */
struct WritableMapping final {
    HANDLE object{};
    char* view{};
    /** @summary Release the test's mapping and writable view without touching other files. */
    ~WritableMapping() { if(view)UnmapViewOfFile(view);if(object)CloseHandle(object); }
};
/** @summary Require capture to reject a preexisting writable mapped input.
 * @param root Synthetic source root; no external process or crash is involved.
 */
void MappedWriter(const std::filesystem::path& root) {
    Package(root);
    HANDLE file=CreateFileW((root/"scripts/helper.luau").c_str(),GENERIC_READ|GENERIC_WRITE,
        FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
    Check(file!=INVALID_HANDLE_VALUE,"Mapped writer fixture file unavailable");
    WritableMapping mapping;
    mapping.object=CreateFileMappingW(file,nullptr,PAGE_READWRITE,0,0,nullptr);
    const auto mappingError=GetLastError();CloseHandle(file);
    if(!mapping.object)throw std::runtime_error("CreateFileMapping failed: "+std::to_string(mappingError));
    mapping.view=static_cast<char*>(MapViewOfFile(mapping.object,FILE_MAP_WRITE,0,0,0));
    Check(mapping.view!=nullptr,"Writable mapping fixture view unavailable");
    Reject([&]{static_cast<void>(mods::CapturedCandidateTestAccess::Capture(root,mods::InstallationBinding::Create(),Pin(),
        [&](mods::CandidateCapturePhase phase) {
            if(phase==mods::CandidateCapturePhase::AfterLeases) {
                mapping.view[8]='C';
                Check(FlushViewOfFile(mapping.view,0)!=FALSE,"Mapped fixture flush failed");
            }
        }));},"candidate.lease");
}
/** @summary Exercise explicit unsupported formats, resource budgets, aliases and file kinds. @param root Owned fixture root. */
void Invalid(const std::filesystem::path& root) {
    Package(root);const auto binding=mods::InstallationBinding::Create();
    scripting::ScriptDefinition missing;
    missing.resourceAliases.emplace("old",core::Tgi{1,2,3});
    missing.propertyAliases.emplace("old",mods::PropertyPatch{});
    std::string error;
    Check(mods::LoadScriptSymbols(root/"absent",missing,error) && missing.resourceAliases.empty() && missing.propertyAliases.contains("old"),"Missing-manifest legacy alias semantics changed");
    Reject([&]{static_cast<void>(mods::CapturedCandidate::Capture(root,{},Pin()));},"candidate.installation");
    auto bad=Pin();bad.files.erase("scripts/init.luau");Reject([&]{static_cast<void>(mods::CapturedCandidate::Capture(root,binding,bad));},"candidate.base-pin");
    bad=Pin();bad.files["scripts/init.luau"]=std::string(64,'a');Reject([&]{static_cast<void>(mods::CapturedCandidate::Capture(root,binding,bad));},"candidate.base-pin");
    for(const auto* prefix:{"dependencies=[]\n","resources={}\n","services={}\n","native={}\n","contract='simoder.raw-resources'\n","unknown=true\n"}) {
        Write(root/"mod.toml",std::string(prefix)+Manifest());Reject([&]{static_cast<void>(mods::CapturedCandidate::Capture(root,binding,Pin()));},"candidate.manifest-field");
    }
    auto manifest=Manifest();manifest.replace(manifest.find("language='luau'"),15,"language='typescript'");
    Write(root/"mod.toml",manifest);Reject([&]{static_cast<void>(mods::CapturedCandidate::Capture(root,binding,Pin()));});
    Write(root/"mod.toml",Manifest()+"\n[[symbols.resources]]\nname='asset'\ntype=1\ngroup=2\ninstance=3\nsource='synthetic'\n");
    Reject([&]{static_cast<void>(mods::CapturedCandidate::Capture(root,binding,Pin()));},"candidate.patches");
    Package(root);Write(root/"project.toml","retired");Reject([&]{static_cast<void>(mods::CapturedCandidate::Capture(root,binding,Pin()));},"candidate.ambiguous-manifest");std::filesystem::remove(root/"project.toml");
    Write(root/"scripts/extra.dll","not native code");Reject([&]{static_cast<void>(mods::CapturedCandidate::Capture(root,binding,Pin()));},"candidate.script-kind");std::filesystem::remove(root/"scripts/extra.dll");
    Write(root/"scripts/Upper.luau","return 1");Reject([&]{static_cast<void>(mods::CapturedCandidate::Capture(root,binding,Pin()));},"candidate.path");std::filesystem::remove(root/"scripts/Upper.luau");
    std::filesystem::rename(root/"mod.toml",root/"temporary.toml");std::filesystem::rename(root/"temporary.toml",root/"MOD.TOML");
    Reject([&]{static_cast<void>(mods::CapturedCandidate::Capture(root,binding,Pin()));},"candidate.path");
    std::filesystem::rename(root/"MOD.TOML",root/"temporary.toml");std::filesystem::rename(root/"temporary.toml",root/"mod.toml");
    std::filesystem::rename(root/"scripts",root/"temporary");std::filesystem::rename(root/"temporary",root/"Scripts");
    Reject([&]{static_cast<void>(mods::CapturedCandidate::Capture(root,binding,Pin()));},"candidate.path");
    std::filesystem::rename(root/"Scripts",root/"temporary");std::filesystem::rename(root/"temporary",root/"scripts");
    Write(root/"scripts/helper.luau",std::string(1024*1024+1,' '));Reject([&]{static_cast<void>(mods::CapturedCandidate::Capture(root,binding,Pin()));},"candidate.file-size");Package(root);
    Write(root/"mod.toml",std::string(1024*1024+1,' '));Reject([&]{static_cast<void>(mods::CapturedCandidate::Capture(root,binding,Pin()));},"candidate.file-size");Package(root);
    Check(CreateHardLinkW((root/"scripts/linked.luau").c_str(),(root/"scripts/helper.luau").c_str(),nullptr)!=FALSE,"Hardlink fixture unavailable");
    Reject([&]{static_cast<void>(mods::CapturedCandidate::Capture(root,binding,Pin()));},"candidate.file-kind");std::filesystem::remove(root/"scripts/linked.luau");
    Write(root/"scripts/helper.luau","this is not valid Luau ???");Reject([&]{static_cast<void>(mods::CapturedCandidate::Capture(root,binding,Pin()));});Package(root);
    for(unsigned i=0;i<255;++i)Write(root/("scripts/m"+std::to_string(i)+".luau"),"return 1");
    Reject([&]{static_cast<void>(mods::CapturedCandidate::Capture(root,binding,Pin()));},"candidate.file-count");
}
/** @summary Exercise bounded inventories, source totals, path depth and actual compiler output overflow.
 * @param root Owned synthetic fixture root.
 */
void Budgets(const std::filesystem::path& root) {
    const auto binding=mods::InstallationBinding::Create();
    const auto depth=root/"depth";Package(depth);
    auto nested=depth/"scripts";
    for(unsigned i=0;i<16;++i)nested/="d";
    Write(nested/"x.luau","return 1");
    Reject([&]{static_cast<void>(mods::CapturedCandidate::Capture(depth,binding,Pin()));},"candidate.path");
    const auto dirs=root/"dirs";Package(dirs);
    for(unsigned i=0;i<64;++i)std::filesystem::create_directories(dirs/("scripts/d"+std::to_string(i)));
    Reject([&]{static_cast<void>(mods::CapturedCandidate::Capture(dirs,binding,Pin()));},"candidate.directory-count");
    const auto bytes=root/"bytes";Package(bytes);
    const std::string source(1024*1024,' ');
    for(unsigned i=0;i<9;++i)Write(bytes/("scripts/s"+std::to_string(i)+".luau"),source);
    Reject([&]{static_cast<void>(mods::CapturedCandidate::Capture(bytes,binding,Pin()));},"candidate.source-limit");
    const auto symbols=root/"symbols";Package(symbols);auto manifest=Manifest();
    for(unsigned i=0;i<4096;++i)manifest+="\n[[symbols.resources]]\nname='r"+std::to_string(i)+"'\ntype=1\ngroup=2\ninstance=3\nsource='synthetic'\n";
    Write(symbols/"mod.toml",manifest);Reject([&]{static_cast<void>(mods::CapturedCandidate::Capture(symbols,binding,Pin()));},"candidate.symbol-count");
    const auto link=root/"links";Package(link);Junction(link/"scripts/linked",root/"target");
    Reject([&]{static_cast<void>(mods::CapturedCandidate::Capture(link,binding,Pin()));},"candidate.file-kind");
    Check(RemoveDirectoryW((link/"scripts/linked").c_str())!=FALSE,"Junction cleanup failed");
    const auto traversal=root/"traversal";Package(traversal);manifest=Manifest();
    constexpr std::string_view originalEntry="scripts/main.luau";
    manifest.replace(manifest.find(originalEntry),originalEntry.size(),"scripts/../main.luau");
    Write(traversal/"mod.toml",manifest);Reject([&]{static_cast<void>(mods::CapturedCandidate::Capture(traversal,binding,Pin()));},"candidate.path");
    const auto output=root/"output";Package(output);
    std::string calls;calls.reserve(1024*1024);
    for(unsigned i=0;i<262144;++i)calls+="f()\n";
    Write(output/"scripts/main.luau",calls);
    for(unsigned i=0;i<6;++i)Write(output/("scripts/s"+std::to_string(i)+".luau"),calls);
    Reject([&]{static_cast<void>(mods::CapturedCandidate::Capture(output,binding,Pin()));},"candidate.output-limit");
}
}
/** @summary Run isolated capture/VM qualification without manager, game or authority providers. @return Zero only on success. */
int main() {
    try { Fixture fixture;Immutable(fixture.root/"immutable");Races(fixture.root/"races");MappedWriter(fixture.root/"mapped");Invalid(fixture.root/"invalid");Budgets(fixture.root/"budgets");
        std::cout<<checks<<" immutable candidate checks passed\n";return 0;
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}
