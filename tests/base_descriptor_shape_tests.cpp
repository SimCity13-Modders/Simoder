/** @file @summary Pure descriptor DATA tests; never opens source bytes, console, staging, network or VM. */
#include "platform/base_artifact_reader.hpp"
#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
namespace artifact=sc13::platform::base_artifact;
/** @summary Fail an actual finite-shape assertion; true never denotes source authenticity. */
void Check(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
/** @summary Return independently authored canonical finite DATA, not an authenticated archive. */
std::array<artifact::FilePin,3> Pins(){return {{{"mod.toml",1,{},false},{"scripts/init.luau",1,{},false},{"LICENSE",1,{},true}}};}
/** @summary Qualify exact existing bounds and closed membership, including valid similar prefixes. */
int main(){try{auto files=Pins();artifact::TrustedArtifact selected{2048,{},files};Check(artifact::ValidateTrustedArtifact(selected),"original finite shape accepted");
    for(const auto size:{0U,512U,2049U,8389120U}){auto changed=selected;changed.size=size;Check(!artifact::ValidateTrustedArtifact(changed),"archive extent rejected");}
    auto changed=selected;changed.size=static_cast<std::uint32_t>(artifact::MaximumArtifact);Check(artifact::ValidateTrustedArtifact(changed),"exact8MiB extent remains shape-valid");
    for(const auto name:{"../mod.toml","scripts/../init.luau","scripts/con.luau","scripts/init.luau:stream","scripts/Init.luau","scripts/a\\b.luau","scripts/a..luau."}){files=Pins();files[1].path=name;Check(!artifact::ValidateTrustedArtifact(selected),"invalid canonical source path refused");}
    files=Pins();files[2].notice=false;Check(!artifact::ValidateTrustedArtifact(selected),"notice omission refused");files=Pins();files[2].size=65537;Check(!artifact::ValidateTrustedArtifact(selected),"notice overflow refused");files[2].size=65536;Check(artifact::ValidateTrustedArtifact(selected),"exact notice cap preserved");
    files=Pins();files[0].size=0;files[1].size=4194304;Check(artifact::ValidateTrustedArtifact(selected),"exact executable cap preserved");files[0].size=1;Check(!artifact::ValidateTrustedArtifact(selected),"aggregate executable overflow refused");
    files=Pins();std::vector<artifact::FilePin> duplicate(files.begin(),files.end());duplicate.push_back(files[1]);auto map=selected;map.files=duplicate;Check(!artifact::ValidateTrustedArtifact(map),"duplicate original path refused");
    auto ancestry=duplicate;ancestry.back().path="scripts/init.luau/lib.luau";map.files=ancestry;Check(!artifact::ValidateTrustedArtifact(map),"file ancestor refused");std::swap(ancestry[1],ancestry.back());Check(!artifact::ValidateTrustedArtifact(map),"reverse order file ancestor refused");
    ancestry=duplicate;ancestry.back().path="scripts/init.luau-extra.luau";map.files=ancestry;Check(artifact::ValidateTrustedArtifact(map),"similar non-boundary prefix preserved");ancestry.push_back({"scripts/init.luau/lib.luau",1,{},false});map.files=ancestry;Check(!artifact::ValidateTrustedArtifact(map),"intervening similar prefix cannot hide ancestor");
    files=Pins();std::vector<std::string> names;names.reserve(63);for(int i=0;i<63;++i)names.push_back("scripts/m"+std::to_string(i)+".luau");std::vector<artifact::FilePin> many(files.begin(),files.end());for(int i=0;i<62;++i)many.push_back({names[static_cast<std::size_t>(i)],0,{},false});map.files=many;Check(artifact::ValidateTrustedArtifact(map),"65total64executable exact cap preserved");many.push_back({names[62],0,{},false});map.files=many;Check(!artifact::ValidateTrustedArtifact(map),"66member cap rejected");
    std::string boundary="scripts/"+std::string(90,'a')+"/"+std::string(90,'b')+"/"+std::string(45,'c')+".luau";Check(boundary.size()==240,"exact path boundary authored");many.assign(files.begin(),files.end());many.push_back({boundary,0,{},false});map.files=many;Check(artifact::ValidateTrustedArtifact(map),"exact240 path preserved");boundary.insert(boundary.size()-5,1,'c');many.back().path=boundary;Check(!artifact::ValidateTrustedArtifact(map),"241path refused");
    std::cout<<"PASS pure descriptor DATA bounds; no authenticity or authority claimed\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
