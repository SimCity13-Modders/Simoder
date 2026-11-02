#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace sc13::bootstrap::detail {
/** @summary Pure observation of one actual WinHTTP response; accepted never authorizes download, installation or execution. */
struct BaseResponsePolicy final {
    bool accepted{};
    std::size_t redirect{4};
};
/** @summary Inspect bounded CRLF headers returned by WinHTTP using the independently queried numeric status.
 * @param raw Successful WINHTTP_QUERY_RAW_HEADERS_CRLF string, excluding its terminating UTF16 NUL.
 * @param status Numeric status queried from the SAME actual request, not parsed from a release manifest.
 * @param expectedBytes Independently pinned archive size, whose exact body is still verified by the actual reader.
 * @param destinations Already validated complete selected URLs; these are expectations, never URL-derived authority.
 * @param visited Previously attempted destination bits for this original request chain.
 * @param requestIndex Original bounded request ordinal; no retry or new slot is created here.
 * @return Accepted response or exact selected redirect index. No allocation, IO, normalization or state mutation occurs.
 * @remarks The status line remains WinHTTP-owned formatting; its text is not used as authority or a version selector.
 * Existing limits remain 32767 UTF16 units, 8192 per field, 128 fields, four destinations and four requests. */
inline BaseResponsePolicy InspectBaseResponse(std::wstring_view raw,std::uint32_t status,std::uint32_t expectedBytes,
    std::span<const std::wstring_view> destinations,unsigned visited,std::size_t requestIndex) noexcept {
    constexpr std::size_t HeaderLimit=32767,FieldLimit=8192,FieldCount=128,DestinationLimit=4;
    BaseResponsePolicy result;
    if(raw.empty()||raw.size()>HeaderLimit||raw.find(L'\0')!=raw.npos||!raw.ends_with(L"\r\n\r\n")||
        !expectedBytes||expectedBytes>8388608||destinations.size()>DestinationLimit||requestIndex>=DestinationLimit)return result;
    /** @summary Compare only ASCII header syntax, without locale or allocating lowercase copies. */
    const auto field=[](std::wstring_view a,std::wstring_view b)noexcept{
        if(a.size()!=b.size())return false;
        for(std::size_t i=0;i<a.size();++i){auto c=a[i];if(c>=L'A'&&c<=L'Z')c+=32;if(c!=b[i])return false;}return true;
    };
    /** @summary Preserve the original optional whitespace grammar; never trim a redirect beyond HTTP OWS. */
    const auto trim=[](std::wstring_view v)noexcept{
        while(!v.empty()&&(v.front()==L' '||v.front()==L'\t'))v.remove_prefix(1);
        while(!v.empty()&&(v.back()==L' '||v.back()==L'\t'))v.remove_suffix(1);return v;
    };
    /** @summary Decode a bounded exact decimal length with the unchanged archive maximum and no wrapping. */
    const auto length=[](std::wstring_view v,std::uint32_t& output)noexcept{
        if(v.empty()||v.size()>10)return false;std::uint64_t n=0;
        for(auto c:v){if(c<L'0'||c>L'9')return false;n=n*10+unsigned(c-L'0');if(n>8388608)return false;}
        output=static_cast<std::uint32_t>(n);return true;
    };
    const auto first=raw.find(L"\r\n");if(first==raw.npos)return result;
    std::size_t start=first+2,fields=0;bool hasLength=false,hasLocation=false,hasEncoding=false,hasTransfer=false,terminated=false;
    std::uint32_t contentLength{};
    while(start<raw.size()){
        const auto end=raw.find(L"\r\n",start);if(end==raw.npos)return {};
        if(end==start){if(end+2!=raw.size())return {};terminated=true;break;}
        auto row=raw.substr(start,end-start);
        if(row.size()>FieldLimit||++fields>FieldCount||row.front()==L' '||row.front()==L'\t')return {};
        const auto colon=row.find(L':');if(colon==row.npos||!colon)return {};
        const auto key=row.substr(0,colon),value=trim(row.substr(colon+1));
        for(auto c:key)if(!((c>=L'a'&&c<=L'z')||(c>=L'A'&&c<=L'Z')||(c>=L'0'&&c<=L'9')||c==L'-'))return {};
        for(auto c:value)if((c<32&&c!=L'\t')||c>126)return {};
        if(field(key,L"content-length")){if(hasLength||!length(value,contentLength))return {};hasLength=true;}
        else if(field(key,L"location")){
            if(hasLocation||value.empty()||value.size()>8192)return {};hasLocation=true;
            for(std::size_t n=0;n<destinations.size();++n)if(value==destinations[n])result.redirect=n;
        }
        else if(field(key,L"content-encoding")){if(hasEncoding||!field(value,L"identity"))return {};hasEncoding=true;}
        else if(field(key,L"transfer-encoding")){if(hasTransfer||!field(value,L"chunked"))return {};hasTransfer=true;}
        start=end+2;
    }
    if(!terminated||(hasTransfer&&hasLength))return {};
    if(status==200)result.accepted=(!hasLength||contentLength==expectedBytes)&&!hasLocation;
    else if(status==301||status==302||status==303||status==307||status==308)
        result.accepted=hasLocation&&result.redirect<destinations.size()&&!(visited&(1U<<result.redirect))&&requestIndex<3;
    if(!result.accepted)return {};return result;
}
}
