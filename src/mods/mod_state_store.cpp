#include "mods/mod_state_store.hpp"

#include "formats/toon/toon_document.hpp"
#include "mods/mod_definition_parser.hpp"
#include "platform/file_publication_retry.hpp"

#include <Windows.h>

#include <fstream>
#include <set>
#include <algorithm>
#include <limits>
#include <numeric>

namespace sc13::mods {
namespace state_detail {
bool Serialize(std::span<const ModId> enabled,TextSink sink,void* context,std::size_t maximum,std::string& error){
    if(maximum!=std::numeric_limits<std::size_t>::max()&&enabled.size()>524288){error="Enabled-state input exceeds bounded serializer indices";return false;}
    std::vector<std::size_t> order(enabled.size());std::iota(order.begin(),order.end(),0);
    for(const auto& id:enabled)if(!IsValidModId(id)){error="Enabled-state save received an invalid or duplicate mod ID";return false;}
    std::sort(order.begin(),order.end(),[&](auto a,auto b){return enabled[a]<enabled[b];});
    for(std::size_t i=1;i<order.size();++i)if(enabled[order[i-1]]==enabled[order[i]]){error="Enabled-state save received an invalid or duplicate mod ID";return false;}
    std::size_t total{};const auto append=[&](std::string_view value){if(value.size()>maximum-total)return false;if(!sink(context,value))return false;total+=value.size();return true;};
    if(!append("enabled[")||!append(std::to_string(enabled.size()))||!append(enabled.empty()? "]:\n":"]: "))return false;
    for(std::size_t i=0;i<enabled.size();++i)if((i&&!append(","))||!append(enabled[i]))return false;
    return enabled.empty()||append("\n");
}
RetryResult PublishWithRetry(Publication call,void* context) noexcept{
    RetryResult result;const auto start=GetTickCount64();
    for(;;){std::uint32_t error{};Attempt outcome;try{outcome=call(context,error);}catch(...){result.exception=true;break;}if(outcome==Attempt::Stop)break;++result.calls;if(outcome==Attempt::Succeeded){result.success=true;break;}result.error=error;if(outcome==Attempt::StopAfterEffect)break;const auto delay=platform::FilePublicationRetryDelay(error,result.calls,GetTickCount64()-start);if(!delay)break;result.backoffMs+=delay;Sleep(delay);if(GetTickCount64()-start>=platform::FilePublicationBudgetMs)break;}
    result.elapsedMs=GetTickCount64()-start;return result;
}
}

bool LoadEnabledState(
    const std::filesystem::path& path,
    std::vector<ModId>& enabled,
    std::string& error) noexcept {
    try {
        enabled.clear();
        error.clear();
        std::error_code existsError;
        const bool exists = std::filesystem::exists(path, existsError);
        if (existsError) {
            error = "Could not query state.toon";
            return false;
        }
        if (!exists) {
            return true;
        }
        formats::toon::Document document;
        formats::toon::ParseError parseError;
        if (!formats::toon::LoadFile(path, document, parseError)) {
            error = "state.toon";
            if (parseError.line != 0U) {
                error += ":" + std::to_string(parseError.line);
            }
            error += ": " + parseError.message;
            return false;
        }
        const auto field = document.root().find("enabled");
        if (field == document.root().end() || field->second.AsArray() == nullptr) {
            error = "state.toon requires an enabled array";
            return false;
        }
        std::set<ModId, std::less<>> unique;
        std::vector<ModId> parsed;
        for (const formats::toon::Value& value : *field->second.AsArray()) {
            const std::string* const id = value.AsString();
            if (id == nullptr || !IsValidModId(*id)) {
                error = "state.toon contains an invalid mod ID";
                return false;
            }
            if (!unique.insert(*id).second) {
                error = "state.toon contains a duplicate mod ID";
                return false;
            }
            parsed.push_back(*id);
        }
        enabled = std::move(parsed);
        return true;
    } catch (...) {
        enabled.clear();
        error = "Enabled-state load failed due to an allocation exception";
        return false;
    }
}

bool SaveEnabledState(
    const std::filesystem::path& path,
    std::span<const ModId> enabled,
    std::string& error) noexcept {
    try {
        error.clear();
        std::string text;
        struct Text {std::string& text;const std::filesystem::path& path;std::string& error;bool parentReady{};} sink{text,path,error};
        const auto append=[](void* context,std::string_view bytes){auto& sink=*static_cast<Text*>(context);if(!sink.parentReady){std::error_code directoryError;std::filesystem::create_directories(sink.path.parent_path(),directoryError);if(directoryError){sink.error="Could not create the state.toon parent directory";return false;}sink.parentReady=true;}sink.text.append(bytes);return true;};
        if(!state_detail::Serialize(enabled,append,&sink,std::numeric_limits<std::size_t>::max(),error))return false;
        const std::filesystem::path temporary = path.wstring() + L".tmp";
        {
            std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
            if (!stream) {
                error = "Could not create temporary state.toon";
                return false;
            }
            stream.write(text.data(), static_cast<std::streamsize>(text.size()));
            stream.flush();
            if (!stream) {
                error = "Could not write complete temporary state.toon";
                return false;
            }
        }
        struct Legacy {const std::filesystem::path& temporary;const std::filesystem::path& path;} legacy{temporary,path};
        const auto callback=[](void* context,std::uint32_t& error){auto& state=*static_cast<Legacy*>(context);if(MoveFileExW(state.temporary.c_str(),state.path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))return state_detail::Attempt::Succeeded;error=GetLastError();return state_detail::Attempt::Failed;};
        const auto publication=state_detail::PublishWithRetry(callback,&legacy);if(publication.success)return true;
        if(publication.exception){error="Enabled-state save failed due to an allocation exception";return false;}
        DeleteFileW(temporary.c_str());
        error = "Could not atomically publish state.toon; Win32 error " + std::to_string(publication.error) +
                "; attempts=" + std::to_string(publication.calls) +
                "; elapsedMs=" + std::to_string(publication.elapsedMs);
        return false;
    } catch (...) {
        error = "Enabled-state save failed due to an allocation exception";
        return false;
    }
}

}  // namespace sc13::mods
