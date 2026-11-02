/** @file @summary Pure actual acquisition response-policy cases; no console, filesystem, network or runtime issuer. */
#include "bootstrap/base_response_policy.hpp"
#include <array>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using sc13::bootstrap::detail::InspectBaseResponse;
std::size_t checks{};
constexpr std::array<std::wstring_view,4> Destinations{L"https://release.example/a",L"https://release.example/b",
    L"https://mirror.example/c",L"https://mirror.example/d"};
/** @summary Fail one exact immutable oracle without retry or external effects. */
void Check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
/** @summary Supply the actual WinHTTP raw string shape with its separate trusted numeric status. */
auto Observe(std::wstring_view text,std::uint32_t status=200,unsigned visited=0,std::size_t ordinal=0){
    return InspectBaseResponse(text,status,1024,Destinations,visited,ordinal);
}
/** @summary Preserve valid identity, close-delimited, chunked and optional-whitespace response semantics. */
void Positive(){
    Check(Observe(L"HTTP/1.1 200 OK\r\n\r\n").accepted,"empty header section remains valid");
    Check(Observe(L"HTTP/1.1 200 OK\r\nContent-Length: 1024\r\n\r\n").accepted,"exact pinned length");
    Check(Observe(L"HTTP/1.1 200 OK\r\ncOnTeNt-LeNgTh: \t00001024 \t\r\nCONTENT-ENCODING: IDENTITY\r\n\r\n").accepted,"case and original OWS preserved");
    Check(Observe(L"HTTP/1.1 200 OK\r\nTransfer-Encoding: CHUNKED\r\nX-Trace: a\tb\r\n\r\n").accepted,"chunked and noncritical tab remain valid");
    Check(Observe(L"HTTP/2.0 200\r\nContent-Length: 1024\r\n\r\n").accepted,"WinHTTP status text not a version gate");
    for(const auto status:{301U,302U,303U,307U,308U}){
        const auto value=Observe(L"HTTP/1.1 302 Found\r\nLocation: https://release.example/b\r\n\r\n",status,1,0);
        Check(value.accepted&&value.redirect==1,"every existing allowed redirect status selects exact expected URL");
    }
    Check(Observe(L"HTTP/1.1 302 Found\r\nLocation: https://mirror.example/d\r\n\r\n",302,7,2).accepted,"fourth destination at third request still valid");
    Check(Observe(L"HTTP/1.1 200 OK\r\nContent-Length: 1024\r\n\r\n",200,15,3).accepted,"fourth terminal request may succeed");
}
/** @summary Reject duplicate security fields, conflicting framing and every unselected or already-visited redirect. */
void Negative(){
    constexpr std::wstring_view bad[]{
        L"",L"HTTP/1.1 200 OK\r\n",L"HTTP/1.1 200 OK\r\nContent-Length: 1024\r\n",
        L"HTTP/1.1 200 OK\r\nContent-Length: 1024\r\n\r\nextra",
        L"HTTP/1.1 200 OK\r\nContent-Length: 1024\r\nContent-Length: 1024\r\n\r\n",
        L"HTTP/1.1 200 OK\r\nContent-Length: 1024\r\ncOnTeNt-LeNgTh: 1024\r\n\r\n",
        L"HTTP/1.1 200 OK\r\nContent-Length: 1024\r\nTransfer-Encoding: chunked\r\n\r\n",
        L"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n\r\n",
        L"HTTP/1.1 200 OK\r\nContent-Encoding: identity\r\nContent-Encoding: identity\r\n\r\n",
        L"HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\n\r\n",
        L"HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n",
        L"HTTP/1.1 200 OK\r\nContent-Length: 1023\r\n\r\n",
        L"HTTP/1.1 200 OK\r\nContent-Length: 1025\r\n\r\n",
        L"HTTP/1.1 200 OK\r\nContent-Length: 8388609\r\n\r\n",
        L"HTTP/1.1 200 OK\r\nContent-Length: 999999999999999999999\r\n\r\n",
        L"HTTP/1.1 200 OK\r\nContent-Length: +1024\r\n\r\n",
        L"HTTP/1.1 200 OK\r\nContent-Length: 1024,1024\r\n\r\n",
        L"HTTP/1.1 200 OK\r\nContent-Length:\r\n\r\n",
        L"HTTP/1.1 200 OK\r\nLocation: https://release.example/b\r\n\r\n",
        L"HTTP/1.1 200 OK\r\n Content-Length: 1024\r\n\r\n",
        L"HTTP/1.1 200 OK\r\nX: valid\r\n\tcontinued\r\n\r\n",
        L"HTTP/1.1 200 OK\r\nX_Name: valid\r\n\r\n",
        L"HTTP/1.1 200 OK\r\n: valid\r\n\r\n",
        L"HTTP/1.1 200 OK\r\nNoColon\r\n\r\n",
        L"HTTP/1.1 200 OK\r\nX: \x01\r\n\r\n",
        L"HTTP/1.1 200 OK\r\nX: \x80\r\n\r\n"};
    for(const auto text:bad)Check(!Observe(text).accepted,"malformed or conflicting response refused");
    for(const auto status:{100U,201U,204U,300U,304U,305U,306U,400U,401U,404U,500U})
        Check(!Observe(L"HTTP/1.1 200 OK\r\n\r\n",status).accepted,"separate actual unsupported status refused");
    constexpr std::wstring_view redirects[]{L"https://release.example/b/extra",L"/b",L"http://release.example/b",L"https://Release.example/b",L"https://release.example/b?extra",L"https://release.example/%62"};
    for(const auto url:redirects){const auto raw=L"HTTP/1.1 302 Found\r\nLocation: "+std::wstring(url)+L"\r\n\r\n";
        Check(!Observe(raw,302).accepted,"redirect requires exact already-selected complete spelling");}
    Check(!Observe(L"HTTP/1.1 302 Found\r\nLocation: https://release.example/b\r\n\r\n",302,2).accepted,"visited redirect refused");
    Check(!Observe(L"HTTP/1.1 302 Found\r\nLocation: https://release.example/b\r\n\r\n",302,0,3).accepted,"fifth request not admitted");
    Check(!Observe(L"HTTP/1.1 302 Found\r\nLocation: https://release.example/b\r\nLocation: https://release.example/b\r\n\r\n",302).accepted,"duplicate Location refused");
    Check(!Observe(L"HTTP/1.1 302 Found\r\n\r\n",302).accepted,"redirect without location refused");
    auto nul=std::wstring(L"HTTP/1.1 200 OK\r\nX: a");nul.push_back(0);nul+=L"b\r\n\r\n";
    Check(!Observe(nul).accepted,"embedded UTF16 NUL refused");
}
/** @summary Exercise inclusive original row/count/raw caps and finite caller bounds without changing resource limits. */
void Boundaries(){
    std::wstring row=L"HTTP/1.1 200 OK\r\nX:"+std::wstring(8190,L'a')+L"\r\n\r\n";
    Check(Observe(row).accepted,"8192-unit field accepted");row.insert(row.size()-4,1,L'a');Check(!Observe(row).accepted,"8193-unit field refused");
    std::wstring fields=L"HTTP/1.1 200 OK\r\n";for(unsigned n=0;n<128;++n)fields+=L"X: a\r\n";
    Check(Observe(fields+L"\r\n").accepted,"128 fields accepted");Check(!Observe(fields+L"X: a\r\n\r\n").accepted,"129 fields refused");
    std::wstring raw=L"HTTP/1.1 200 OK\r\n";
    while(raw.size()+8194+2<=32767)raw+=L"X:"+std::wstring(8190,L'a')+L"\r\n";
    const auto remaining=32767-raw.size()-2;raw+=L"X:"+std::wstring(remaining-4,L'a')+L"\r\n\r\n";
    Check(raw.size()==32767&&Observe(raw).accepted,"32767-unit raw API string accepted");
    raw.insert(raw.size()-4,1,L'a');Check(!Observe(raw).accepted,"32768-unit raw string refused");
    const auto empty=L"HTTP/1.1 200 OK\r\n\r\n";
    Check(!InspectBaseResponse(empty,200,0,Destinations,0,0).accepted,"zero expected archive refused");
    Check(!InspectBaseResponse(empty,200,8388609,Destinations,0,0).accepted,"archive cap unchanged");
    Check(InspectBaseResponse(empty,200,8388608,Destinations,0,0).accepted,"archive upper bound retained");
    Check(!Observe(empty,200,0,4).accepted,"out of range ordinal refused");
    std::array<std::wstring_view,5> excessive{};Check(!InspectBaseResponse(empty,200,1024,excessive,0,0).accepted,"fifth selected destination refused");
}
}
/** @summary Execute pure production policy only; authenticated body capture and actual WinHTTP transport remain outside this test. */
int main(){try{Positive();Negative();Boundaries();std::cout<<"Base response policy pure checks passed: "<<checks<<'\n';return 0;}
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
