#include "bootstrap/base_dependency.hpp"
#include "bootstrap/base_response_policy.hpp"
#include <winhttp.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <cwchar>
#include <limits>
#include <mutex>
#include <new>

#pragma comment(lib,"winhttp.lib")
namespace sc13::bootstrap {
namespace {
constexpr std::uint64_t DecisionMs=600000, AcquisitionMs=120000, CloseObservationMs=5000;
constexpr std::size_t UrlLimit=8192, PathLimit=32760, StagingPathLimit=4096, HeaderUnits=32768;
using Snapshot=platform::base_artifact::Snapshot;
using Artifact=platform::base_artifact::TrustedArtifact;
/** @summary Reject malformed UTF16, embedded control input and unbounded metadata before ownership staging. */
bool Text(std::wstring_view value,std::size_t maximum,bool allowEmpty=false) noexcept {
    if((value.empty()&&!allowEmpty)||value.size()>maximum)return false;
    for(std::size_t n=0;n<value.size();++n){const auto c=value[n];
        if(c<32||c==127)return false;
        if(c>=0xD800&&c<=0xDBFF){if(++n==value.size()||value[n]<0xDC00||value[n]>0xDFFF)return false;}
        else if(c>=0xDC00&&c<=0xDFFF)return false;
    }return true;
}
/** @summary Fixed local ordinary drive syntax only; reader independently qualifies actual retained ancestry. */
bool LocalPath(std::wstring_view value) noexcept {
    if(!Text(value,PathLimit)||value.size()<4||!((value[0]>=L'A'&&value[0]<=L'Z')||(value[0]>=L'a'&&value[0]<=L'z'))||value[1]!=L':'||value[2]!=L'\\')return false;
    std::size_t components=0,start=3;
    for(std::size_t n=3;n<=value.size();++n){
        if(n<value.size()&&(value[n]==L'/'||value[n]==L':'||value[n]==L'"'||value[n]==L'*'||value[n]==L'?'||value[n]==L'<'||value[n]==L'>'||value[n]==L'|'))return false;
        if(n==value.size()||value[n]==L'\\'){
            const auto component=value.substr(start,n-start);
            if(component.empty()||component==L"."||component==L".."||component.back()==L'.'||component.back()==L' '||++components>66)return false;
            start=n+1;
        }
    }wchar_t drive[]{value[0],L':',L'\\',0};return GetDriveTypeW(drive)==DRIVE_FIXED;
}
/** @summary Convert only a previously validated ordinary drive path to internal extended syntax. */
std::wstring Extended(std::wstring_view value){std::wstring result=L"\\\\?\\";result.append(value);return result;}
/** @summary Qualify a nonreparse single-link default stream ordinary file/directory. */
bool Ordinary(HANDLE handle,bool directory,BY_HANDLE_FILE_INFORMATION* output=nullptr) noexcept {
    BY_HANDLE_FILE_INFORMATION info{};
    if(handle==INVALID_HANDLE_VALUE||GetFileType(handle)!=FILE_TYPE_DISK||!GetFileInformationByHandle(handle,&info)||
       (info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)||bool(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=directory||(!directory&&info.nNumberOfLinks!=1))return false;
    if(output)*output=info;return true;
}
/** @summary Compare actual retained NTFS/file identity, never a pathname or timestamp alone. */
bool SameFile(const BY_HANDLE_FILE_INFORMATION& a,const BY_HANDLE_FILE_INFORMATION& b) noexcept {
    return a.dwVolumeSerialNumber==b.dwVolumeSerialNumber&&a.nFileIndexHigh==b.nFileIndexHigh&&a.nFileIndexLow==b.nFileIndexLow&&b.nNumberOfLinks==1;
}
/** @summary Full lower-ASCII canonical HTTPS destination; exact complete URLs are matched separately. */
bool Url(std::wstring_view value,std::array<wchar_t,254>& host,INTERNET_PORT& port,std::wstring_view& path) noexcept {
    if(!Text(value,UrlLimit)||!value.starts_with(L"https://")||value.find(L'#')!=value.npos||value.find(L'\\')!=value.npos)return false;
    const auto slash=value.find(L'/',8);if(slash==value.npos)return false;
    auto authority=value.substr(8,slash-8);if(authority.empty()||authority.find(L'@')!=authority.npos)return false;
    port=443;const auto colon=authority.find(L':');
    if(colon!=authority.npos){auto p=authority.substr(colon+1);authority=authority.substr(0,colon);unsigned number=0;
        if(p.empty()||p.size()>5||p[0]==L'0')return false;
        for(auto c:p){if(c<L'0'||c>L'9')return false;number=number*10+unsigned(c-L'0');}if(!number||number>65535)return false;port=INTERNET_PORT(number);
    }
    if(authority.empty()||authority.size()>253||authority.front()==L'.'||authority.back()==L'.')return false;
    std::size_t label=0;wchar_t previous=0;
    for(auto c:authority){if(c==L'.'){if(!label||previous==L'-')return false;label=0;}
        else {if(!((c>=L'a'&&c<=L'z')||(c>=L'0'&&c<=L'9')||c==L'-')||(!label&&c==L'-')||++label>63)return false;}previous=c;}
    if(!label||previous==L'-')return false;
    path=value.substr(slash);for(std::size_t n=0;n<path.size();++n){const auto c=path[n];if(c<33||c>126)return false;if(c==L'%'){
        if(n+2>=path.size())return false;for(unsigned k=0;k<2;++k){const auto hex=path[++n];if(!((hex>=L'0'&&hex<=L'9')||(hex>=L'a'&&hex<=L'f')||(hex>=L'A'&&hex<=L'F')))return false;}
    }}
    std::copy(authority.begin(),authority.end(),host.begin());host[authority.size()]=0;return true;
}

}

/** @summary Sole original body; all callbacks and readers address this stable allocation until real session unload. */
struct BaseDependency::Body final {
    /** @summary Immutable callback lineage; each real request gets its own original bounded ordinal/context. */
    struct CallbackContext final {Body* body{};std::size_t ordinal{4};HINTERNET handle{};bool closeIssued{},acknowledged{};};
    /** @summary Fixed immutable pin storage removes borrowed selection lifetime from asynchronous operations. */
    struct Pin final {std::array<char,241> path{};platform::base_artifact::FilePin value;};
    std::array<Pin,65> pins{};
    std::array<platform::base_artifact::FilePin,65> map{};
    Artifact trusted{};
    std::array<std::array<wchar_t,UrlLimit+1>,4> urls{};
    std::wstring line;
    std::array<wchar_t,2048> prompt{};
    std::array<wchar_t,HeaderUnits> headers{};
    std::array<std::byte,65536> transfer{};
    std::array<INPUT_RECORD,32> events{};
    std::array<HANDLE,65> parents{};
    std::array<HINTERNET,4> connections{},requests{};
    std::array<HINTERNET,4> closingConnections{};
    std::array<HINTERNET,4> closingRequests{};
    std::array<bool,4> requestClosed{};
    std::array<CallbackContext,4> requestContexts{};
    CallbackContext parentContext{};
    std::mutex callbackMutex;
    std::atomic<bool> cancelled{};
    std::atomic<std::uint32_t> readers{};
    const std::atomic<std::uint64_t>* ownerEpoch{};
    std::uint64_t admissionEpoch{};
    HANDLE input{INVALID_HANDLE_VALUE},output{INVALID_HANDLE_VALUE},stage{INVALID_HANDLE_VALUE},unloadEvent{};
    HINTERNET session{};
    HINTERNET closingSession{};
    BY_HANDLE_FILE_INFORMATION stageIdentity{};
    std::filesystem::path stagePath;
    std::shared_ptr<const Snapshot> capture;
    BaseDependencyState state{BaseDependencyState::Decision};
    BaseDependencyError error{};
    std::uint64_t generation{},decisionStart{},acquireStart{},closeStart{};
    std::size_t count{},urlCount{},lineSize{},parentCount{},requestIndex{};
    std::size_t nextUrlIndex{};
    unsigned visitedUrls{};
    std::uint32_t received{},completionLength{};
    DWORD completion{},networkError{};
    unsigned informational{};
    bool callbackInstalled{},unloadConfigured{},closing{},closeFault{},networkDone{},stageCreated{},stageSealed{},stageDeleted{},terminal{},inputCarriage{},overflow{},capturePublished{};
    /** @summary Documented dynamically resolved NOWAIT console API; no blocking read race is accepted. */
    using ReadEvents=BOOL(WINAPI*)(HANDLE,PINPUT_RECORD,DWORD,LPDWORD,USHORT);
    ReadEvents readEvents{};

    /** @summary No effect construction; parents start invalid so partial staging has deterministic ownership. */
    Body():line(PathLimit+1,L'\0') {parents.fill(INVALID_HANDLE_VALUE);parentContext.body=this;for(std::size_t n=0;n<requestContexts.size();++n){requestContexts[n].body=this;requestContexts[n].ordinal=n;}}
    /** @summary Observe same-original parent stop directly even while this pump is processing one transition. */
    bool Stopped() const noexcept {return cancelled.load()||(ownerEpoch&&(ownerEpoch->load(std::memory_order_acquire)!=admissionEpoch||admissionEpoch==std::numeric_limits<std::uint64_t>::max()));}
    /** @summary Destruction is legal only after Owner settled OS callbacks/staging/readers; parent stops retain this original. */
    ~Body(){for(auto h:parents)if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);if(input!=INVALID_HANDLE_VALUE)CloseHandle(input);if(output!=INVALID_HANDLE_VALUE)CloseHandle(output);if(unloadEvent)CloseHandle(unloadEvent);}
    /** @summary Snapshot exact externally selected content and review before console/network effects. */
    bool Select(const BaseDependencySelection& s,std::uint64_t id) noexcept {
        if(!platform::base_artifact::ValidateTrustedArtifact(s.artifact)||
           !Text(s.version,128)||!Text(s.channel,32)||!Text(s.target,512)||!Text(s.reason,256)||s.urls.size()>4)return false;
        trusted.size=s.artifact.size;trusted.digest=s.artifact.digest;count=s.artifact.files.size();
        for(std::size_t n=0;n<count;++n){const auto& source=s.artifact.files[n];
            // @summary Validator established fixed capacities; copy the SAME borrowed immutable native descriptor into owned backing.
            std::copy(source.path.begin(),source.path.end(),pins[n].path.begin());pins[n].value=source;
            pins[n].value.path={pins[n].path.data(),source.path.size()};map[n]=pins[n].value;
        }
        trusted.files={map.data(),count};
        if(!platform::base_artifact::ValidateTrustedArtifact(trusted))return false;
        urlCount=s.urls.size();
        for(std::size_t n=0;n<urlCount;++n){std::array<wchar_t,254> host{};INTERNET_PORT port{};std::wstring_view path;if(!Url(s.urls[n].exact,host,port,path))return false;
            for(std::size_t j=0;j<n;++j)if(s.urls[n].exact==std::wstring_view(urls[j].data()))return false;
            std::copy(s.urls[n].exact.begin(),s.urls[n].exact.end(),urls[n].begin());
        }
        generation=id;
        std::array<wchar_t,65> hash{};constexpr wchar_t hex[]=L"0123456789ABCDEF";for(std::size_t n=0;n<trusted.digest.size();++n){const auto byte=std::to_integer<unsigned>(trusted.digest[n]);hash[n*2]=hex[byte>>4];hash[n*2+1]=hex[byte&15];}
        const auto written=swprintf_s(prompt.data(),prompt.size(),
            L"\r\nSimoder requires base.\r\nReason: %.*ls\r\nVersion: %.*ls\r\nChannel: %.*ls\r\nTarget: %.*ls\r\nGeneration: %llu\r\nArtifact: %u authenticated bytes\r\nSHA256: %ls\r\nActions: %lsmanual / cancel\r\n> ",
            int(s.reason.size()),s.reason.data(),int(s.version.size()),s.version.data(),int(s.channel.size()),s.channel.data(),int(s.target.size()),s.target.data(),static_cast<unsigned long long>(generation),trusted.size,hash.data(),urlCount?L"install / ":L"");
        return written>0;
    }
    /** @summary Force the actual existing nonactivating console, then retain independent input/output handles. */
    bool Console(logging::AsyncLogger& logger) noexcept {
        if(Stopped())return false;
        logging::DeveloperConsoleOptions options{};options.enabled=true;if(!logger.EnableDeveloperConsole(options))return false;
        const auto kernel=GetModuleHandleW(L"kernel32.dll");const auto proc=kernel?GetProcAddress(kernel,"ReadConsoleInputExW"):nullptr;
        if(!proc)return false;static_assert(sizeof(proc)==sizeof(readEvents));std::memcpy(&readEvents,&proc,sizeof(proc));
        input=CreateFileW(L"CONIN$",GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
        output=CreateFileW(L"CONOUT$",GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
        DWORD mode{};if(input==INVALID_HANDLE_VALUE||output==INVALID_HANDLE_VALUE||!GetConsoleMode(input,&mode)||!GetConsoleMode(output,&mode))return false;
        // @summary Discard pre-review typeahead; no preexisting key event can approve a newly displayed selection.
        if(!FlushConsoleInputBuffer(input))return false;
        DWORD written{};decisionStart=GetTickCount64();return WriteConsoleW(output,prompt.data(),DWORD(wcslen(prompt.data())),&written,nullptr)!=FALSE;
    }
    /** @summary Close only current input/output; ReadConsoleInput is never pending on another thread. */
    void CloseConsole() noexcept {if(input!=INVALID_HANDLE_VALUE){CloseHandle(input);input=INVALID_HANDLE_VALUE;}if(output!=INVALID_HANDLE_VALUE){CloseHandle(output);output=INVALID_HANDLE_VALUE;}}
    /** @summary Read at most32 actual console events without changing global modes or standard streams. */
    bool Input() noexcept {
        if(WaitForSingleObject(input,100)==WAIT_TIMEOUT)return true;
        DWORD read{};if(!readEvents(input,events.data(),DWORD(events.size()),&read,0x0002)||read>events.size()){Fail(BaseDependencyError::Input);return false;}
        for(DWORD n=0;n<read;++n){if(Stopped()||GetTickCount64()-decisionStart>=DecisionMs){Fail(Stopped()?BaseDependencyError::Cancelled:BaseDependencyError::Deadline);return false;}
            const auto& event=events[n];if(event.EventType!=KEY_EVENT||!event.Event.KeyEvent.bKeyDown)continue;
            const auto& key=event.Event.KeyEvent;const auto c=key.uChar.UnicodeChar;
            if(key.wRepeatCount>32){Fail(BaseDependencyError::Input);return false;}
            if((key.dwControlKeyState&(LEFT_CTRL_PRESSED|RIGHT_CTRL_PRESSED))&&(c==3||c==4||c==26)){Fail(BaseDependencyError::Cancelled);return false;}
            if(!c)continue;
            for(WORD repeat=0;repeat<key.wRepeatCount;++repeat){
                if(c==L'\r'||c==L'\n'){
                    if(c==L'\n'&&inputCarriage){inputCarriage=false;continue;}inputCarriage=c==L'\r';
                    if(overflow||!Text({line.data(),lineSize},state==BaseDependencyState::ManualPath?PathLimit:16)){Fail(BaseDependencyError::Input);return false;}
                    if(state==BaseDependencyState::ManualPath){if(!LocalPath({line.data(),lineSize})){Fail(BaseDependencyError::Input);return false;}
                        CloseConsole();try{line.resize(lineSize);CaptureFile(std::filesystem::path(std::move(line)));}catch(...){Fail(BaseDependencyError::Input);}return false;}
                    const std::wstring_view action(line.data(),lineSize);
                    if(action==L"cancel"){Fail(BaseDependencyError::Cancelled);return false;}
                    if(action==L"manual"){state=BaseDependencyState::ManualPath;std::fill(line.begin(),line.end(),L'\0');lineSize=0;DWORD written{};const wchar_t text[]=L"\r\nAbsolute verified artifact file (no quotes):\r\n> ";if(!WriteConsoleW(output,text,DWORD(std::size(text)-1),&written,nullptr)){Fail(BaseDependencyError::Console);return false;}continue;}
                    if(action==L"install"&&urlCount){CloseConsole();StartNetwork();return false;}
                    std::fill(line.begin(),line.end(),L'\0');lineSize=0;DWORD written{};const wchar_t text[]=L"\r\nUse a displayed literal action.\r\n> ";if(!WriteConsoleW(output,text,DWORD(std::size(text)-1),&written,nullptr)){Fail(BaseDependencyError::Console);return false;}
                }else if(c==L'\b'){if(lineSize)line[--lineSize]=0;}
                else {const std::size_t maximum=state==BaseDependencyState::ManualPath?PathLimit:16;if(lineSize>=maximum)overflow=true;else{line[lineSize++]=c;line[lineSize]=0;}}
            }
        }return true;
    }
    /** @summary Retain every ordinary staging ancestor without delete sharing; no foreign ownership is minted. */
    bool Parents(const std::filesystem::path& parent) noexcept {
        const auto& p=parent.native();if(!LocalPath(p)||p.size()>StagingPathLimit)return false;
        try{stagePath=parent;std::size_t end=3;
            for(;;){if(parentCount==parents.size())return false;const auto component=Extended(std::wstring_view(p).substr(0,end));
                HANDLE h=CreateFileW(component.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
                if(!Ordinary(h,true)){if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);return false;}parents[parentCount++]=h;
                if(end==p.size())break;const auto next=p.find(L'\\',end==3?3:end+1);end=next==p.npos?p.size():next;
            }return true;
        }catch(...){return false;}
    }
    /** @summary Create one exclusive ordinary file; no fixed-name overwrite, directory creation, or archive allocation. */
    bool Stage() noexcept {
        try{wchar_t leaf[64]{};if(swprintf_s(leaf,L"base-%lu-%llu.ustar",GetCurrentProcessId(),static_cast<unsigned long long>(generation))<=0)return false;
            stagePath/=leaf;const auto native=Extended(stagePath.native());
            stage=CreateFileW(native.c_str(),GENERIC_WRITE|DELETE|FILE_READ_ATTRIBUTES,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
            if(stage==INVALID_HANDLE_VALUE)return false;stageCreated=true;return Ordinary(stage,false,&stageIdentity);
        }catch(...){return false;}
    }
    /** @summary A failure closes admission immediately, retains original ownership, and initiates once-only close. */
    void Fail(BaseDependencyError why) noexcept {if(error==BaseDependencyError::None)error=why;cancelled.store(true);CloseConsole();BeginClose();}
    /** @summary Configure an essential DWORD WinHTTP option; a refusal is never silently ignored. */
    bool Option(HINTERNET handle,DWORD name,DWORD value) noexcept {return WinHttpSetOption(handle,name,&value,sizeof(value))!=FALSE;}
    /** @summary Start the one original public/direct asynchronous session only after explicit matching console input. */
    void StartNetwork() noexcept {
        if(Stopped()||GetTickCount64()-decisionStart>=DecisionMs){Fail(Stopped()?BaseDependencyError::Cancelled:BaseDependencyError::Deadline);return;}
        acquireStart=GetTickCount64();if(!Stage()){Fail(BaseDependencyError::Stage);return;}
        if(!OpenSession())return;Request(0);
    }
    /** @summary Configure the actual session and last-callback completion event before admitting any request. */
    bool OpenSession() noexcept {
        unloadEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);if(!unloadEvent){Fail(BaseDependencyError::Network);return false;}
        session=WinHttpOpen(L"Simoder-Base/1",WINHTTP_ACCESS_TYPE_NO_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,WINHTTP_FLAG_ASYNC);
        if(!session){Fail(BaseDependencyError::Network);return false;}
        if(!WinHttpSetOption(session,WINHTTP_OPTION_UNLOAD_NOTIFY_EVENT,&unloadEvent,sizeof(unloadEvent))){Fail(BaseDependencyError::UnsupportedOption);return false;}unloadConfigured=true;
        // @summary A session context is set before callback installation so even parent HANDLE_CLOSING has its original owner.
        DWORD_PTR context=reinterpret_cast<DWORD_PTR>(&parentContext);
        if(!WinHttpSetOption(session,WINHTTP_OPTION_CONTEXT_VALUE,&context,sizeof(context))||!WinHttpSetTimeouts(session,5000,10000,10000,10000)||
            !Option(session,WINHTTP_OPTION_SECURE_PROTOCOLS,WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2|WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3)){
            Fail(BaseDependencyError::UnsupportedOption);return false;}
        if(WinHttpSetStatusCallback(session,&Callback,WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS|WINHTTP_CALLBACK_FLAG_HANDLES|WINHTTP_CALLBACK_FLAG_SECURE_FAILURE|WINHTTP_CALLBACK_FLAG_INTERMEDIATE_RESPONSE,0)==WINHTTP_INVALID_STATUS_CALLBACK){Fail(BaseDependencyError::UnsupportedOption);return false;}
        callbackInstalled=true;return true;
    }
    /** @summary Validate the exact selected URL and configure all security/resource options before the first send. */
    void Request(std::size_t index,std::size_t destination=0) noexcept {
        if(Stopped()||GetTickCount64()-acquireStart>=AcquisitionMs||index>=4){Fail(Stopped()?BaseDependencyError::Cancelled:BaseDependencyError::Deadline);return;}
        std::array<wchar_t,254> host{};INTERNET_PORT port{};std::wstring_view path;
        if(destination>=urlCount||(visitedUrls&(1U<<destination))||!Url(urls[destination].data(),host,port,path)){Fail(BaseDependencyError::Url);return;}
        visitedUrls|=1U<<destination;
        requestIndex=index;connections[index]=WinHttpConnect(session,host.data(),port,0);if(!connections[index]){Fail(BaseDependencyError::Network);return;}
        requests[index]=WinHttpOpenRequest(connections[index],L"GET",path.data(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE|WINHTTP_FLAG_ESCAPE_DISABLE|WINHTTP_FLAG_ESCAPE_DISABLE_QUERY);
        const auto h=requests[index];if(!h){Fail(BaseDependencyError::Network);return;}
        requestContexts[index].handle=h;
        DWORD_PTR context=reinterpret_cast<DWORD_PTR>(&requestContexts[index]);
        if(!WinHttpSetOption(h,WINHTTP_OPTION_CONTEXT_VALUE,&context,sizeof(context))||
           !Option(h,WINHTTP_OPTION_DISABLE_FEATURE,WINHTTP_DISABLE_AUTHENTICATION|WINHTTP_DISABLE_COOKIES|WINHTTP_DISABLE_REDIRECTS)||
           !Option(h,WINHTTP_OPTION_REDIRECT_POLICY,WINHTTP_OPTION_REDIRECT_POLICY_NEVER)||
           !Option(h,WINHTTP_OPTION_AUTOLOGON_POLICY,WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH)||
           !Option(h,WINHTTP_OPTION_MAX_RESPONSE_HEADER_SIZE,32768)||!Option(h,WINHTTP_OPTION_MAX_RESPONSE_DRAIN_SIZE,65536)||
           !Option(h,WINHTTP_OPTION_MAX_HTTP_STATUS_CONTINUE,2)||!Option(h,WINHTTP_OPTION_ENABLE_FEATURE,WINHTTP_ENABLE_SSL_REVOCATION)||
           !Option(h,WINHTTP_OPTION_REJECT_USERPWD_IN_URL,TRUE)){Fail(BaseDependencyError::UnsupportedOption);return;}
        informational=0;state=BaseDependencyState::Sending;
        if(Stopped()||GetTickCount64()-acquireStart>=AcquisitionMs){Fail(Stopped()?BaseDependencyError::Cancelled:BaseDependencyError::Deadline);return;}
        if(!WinHttpSendRequest(h,L"Accept: application/octet-stream\r\nAccept-Encoding: identity\r\n",DWORD(-1),WINHTTP_NO_REQUEST_DATA,0,0,context)){Fail(BaseDependencyError::Network);}
    }
    /** @summary Bounded WinHTTP callback; no UI/Core/filesystem/VM/hash/wait or subsequent WinHTTP call occurs here.
     * @param handle Original WinHTTP handle. @param context Same stable original body. @param status Native event.
     * @param data Transient event data. @param length Actual event-data bytes. */
    static void CALLBACK Callback(HINTERNET handle,DWORD_PTR context,DWORD status,void* data,DWORD length) noexcept {
        if(!context)return;auto& lineage=*reinterpret_cast<CallbackContext*>(context);auto& body=*lineage.body;std::lock_guard lock(body.callbackMutex);
        const auto ordinal=lineage.ordinal;const bool request=ordinal<body.requestContexts.size() && &lineage==&body.requestContexts[ordinal];
        if(status==WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING){
            if(!request)return;
            if(lineage.handle!=handle||!lineage.closeIssued||lineage.acknowledged||body.closingRequests[ordinal]!=handle){body.networkError=ERROR_INVALID_DATA;return;}
            lineage.acknowledged=true;body.requestClosed[ordinal]=true;body.closingRequests[ordinal]=nullptr;return;
        }
        if(status==WINHTTP_CALLBACK_STATUS_INTERMEDIATE_RESPONSE){if(++body.informational>2)body.networkError=ERROR_INVALID_DATA;return;}
        if(status==WINHTTP_CALLBACK_STATUS_SECURE_FAILURE){body.networkError=ERROR_WINHTTP_SECURE_FAILURE;return;}
        if(status==WINHTTP_CALLBACK_STATUS_REQUEST_ERROR){body.networkError=length==sizeof(WINHTTP_ASYNC_RESULT)&&data?static_cast<WINHTTP_ASYNC_RESULT*>(data)->dwError:ERROR_INVALID_DATA;return;}
        if(status!=WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE&&status!=WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE&&status!=WINHTTP_CALLBACK_STATUS_READ_COMPLETE)return;
        if(body.closing||!request||ordinal!=body.requestIndex||lineage.acknowledged||lineage.handle!=handle||handle!=body.requests[ordinal]||body.completion){body.networkError=ERROR_INVALID_DATA;return;}
        if(status==WINHTTP_CALLBACK_STATUS_READ_COMPLETE&&(length>body.transfer.size()||(length&&data!=body.transfer.data()))){body.networkError=ERROR_INVALID_DATA;return;}
        body.completion=status;body.completionLength=length;
    }
    /** @summary Close a request exactly once and render it unavailable before the API can invoke a closing callback. */
    void CloseRequest(std::size_t n) noexcept {
        HINTERNET h{};{std::lock_guard lock(callbackMutex);h=requests[n];requests[n]=nullptr;if(h){closingRequests[n]=h;requestContexts[n].closeIssued=true;}}
        if(h&&!WinHttpCloseHandle(h)){closeFault=true;error=BaseDependencyError::Cleanup;state=BaseDependencyState::Unresolved;}
    }
    /** @summary Close original WinHTTP children/parents and retain context/event through OS session unload notification. */
    void BeginClose() noexcept {
        {std::lock_guard lock(callbackMutex);if(closing)return;closing=true;closeStart=GetTickCount64();}
        for(std::size_t n=0;n<4;++n)CloseRequest(n);
        for(std::size_t n=0;n<connections.size();++n)if(connections[n]){const auto h=connections[n];connections[n]=nullptr;closingConnections[n]=h;if(!WinHttpCloseHandle(h)){closeFault=true;error=BaseDependencyError::Cleanup;state=BaseDependencyState::Unresolved;}}
        if(session){auto h=session;session=nullptr;closingSession=h;if(!WinHttpCloseHandle(h)){closeFault=true;error=BaseDependencyError::Cleanup;state=BaseDependencyState::Unresolved;}}
        if(state!=BaseDependencyState::Unresolved)state=BaseDependencyState::Closing;
    }
    /** @summary Strict raw-header shape; refuse folded/duplicate security fields and unknown transfer encoding. */
    bool Headers(DWORD& status,std::size_t& redirect) noexcept {
        const auto h=requests[requestIndex];DWORD bytes=sizeof(headers),index=0;
        if(!WinHttpQueryHeaders(h,WINHTTP_QUERY_RAW_HEADERS_CRLF,WINHTTP_HEADER_NAME_BY_INDEX,headers.data(),&bytes,&index)||bytes>sizeof(headers)-sizeof(wchar_t)||bytes%2||!bytes)return false;
        // @summary Successful WinHttpQueryHeaders byte count excludes the UTF16 terminating NUL.
        const auto units=bytes/sizeof(wchar_t);if(units>=headers.size()||headers[units]!=0)return false;
        std::wstring_view raw(headers.data(),units);if(raw.find(L'\0')!=raw.npos)return false;
        DWORD statusSize=sizeof(status);if(!WinHttpQueryHeaders(h,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&status,&statusSize,WINHTTP_NO_HEADER_INDEX))return false;
        std::array<std::wstring_view,4> destinations{};
        for(std::size_t n=0;n<urlCount;++n)destinations[n]=std::wstring_view(urls[n].data());
        const auto policy=detail::InspectBaseResponse(raw,status,trusted.size,{destinations.data(),urlCount},visitedUrls,requestIndex);
        redirect=policy.redirect;return policy.accepted;
    }
    /** @summary Process exactly one observed completion; request transitions remain on the original owning thread. */
    void Network() noexcept {
        if(Stopped()||GetTickCount64()-acquireStart>=AcquisitionMs){Fail(Stopped()?BaseDependencyError::Cancelled:BaseDependencyError::Deadline);return;}
        DWORD event{},size{},failure{};{std::lock_guard lock(callbackMutex);event=completion;size=completionLength;completion=0;failure=networkError;}
        if(failure){Fail(BaseDependencyError::Network);return;}
        if(!event){Sleep(100);return;}
        auto h=requests[requestIndex];
        if(event==WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE&&state==BaseDependencyState::Sending){state=BaseDependencyState::Headers;if(!WinHttpReceiveResponse(h,nullptr))Fail(BaseDependencyError::Network);return;}
        if(event==WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE&&state==BaseDependencyState::Headers){
            DWORD status{};std::size_t redirect{};if(!Headers(status,redirect)){Fail(BaseDependencyError::Headers);return;}
            if(status!=200){nextUrlIndex=redirect;CloseRequest(requestIndex);state=BaseDependencyState::Connecting;return;}
            state=BaseDependencyState::Reading;if(!WinHttpReadData(h,transfer.data(),DWORD(transfer.size()),nullptr))Fail(BaseDependencyError::Network);return;
        }
        if(event==WINHTTP_CALLBACK_STATUS_READ_COMPLETE&&state==BaseDependencyState::Reading){
            if(!size){if(received!=trusted.size){Fail(BaseDependencyError::Body);return;}
                if(!FlushFileBuffers(stage)){Fail(BaseDependencyError::Stage);return;}
                if(!CloseHandle(stage)){Fail(BaseDependencyError::Stage);return;}stage=INVALID_HANDLE_VALUE;stageSealed=true;
                CaptureFile(stagePath);return;
            }
            if(size>trusted.size-received){Fail(BaseDependencyError::Body);return;}
            DWORD written{};if(!WriteFile(stage,transfer.data(),size,&written,nullptr)||written!=size){Fail(BaseDependencyError::Stage);return;}received+=size;
            if(!WinHttpReadData(h,transfer.data(),DWORD(transfer.size()),nullptr))Fail(BaseDependencyError::Network);return;
        }
        Fail(BaseDependencyError::Network);
    }
    /** @summary Converge manual/network file inputs on the existing authenticated immutable same-byte reader only. */
    void CaptureFile(const std::filesystem::path& file) noexcept {
        if(Stopped()||(acquireStart&&GetTickCount64()-acquireStart>=AcquisitionMs)||(!acquireStart&&GetTickCount64()-decisionStart>=DecisionMs)){Fail(Stopped()?BaseDependencyError::Cancelled:BaseDependencyError::Deadline);return;}
        platform::base_artifact::Error refusal{};capture=Snapshot::Capture(file,trusted,refusal);
        if(!capture){Fail(BaseDependencyError::Capture);return;}
        if(Stopped()||(acquireStart&&GetTickCount64()-acquireStart>=AcquisitionMs)||(!acquireStart&&GetTickCount64()-decisionStart>=DecisionMs)){capture.reset();Fail(BaseDependencyError::Changed);return;}
        networkDone=true;BeginClose();
    }
    /** @summary Delete only still-owned unsealed stage, or exact sealed postimage compared with authenticated snapshot. */
    bool CleanupStage() noexcept {
        if(!stageCreated||stageDeleted)return true;
        if(stage!=INVALID_HANDLE_VALUE){FILE_DISPOSITION_INFO disposition{TRUE};if(!SetFileInformationByHandle(stage,FileDispositionInfo,&disposition,sizeof(disposition)))return false;
            if(!CloseHandle(stage))return false;stage=INVALID_HANDLE_VALUE;stageDeleted=true;return true;}
        if(!stageSealed||!capture)return false;
        try{auto path=Extended(stagePath.native());HANDLE h=CreateFileW(path.c_str(),GENERIC_READ|DELETE|FILE_READ_ATTRIBUTES,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
            if(h==INVALID_HANDLE_VALUE)return GetLastError()==ERROR_FILE_NOT_FOUND;
            BY_HANDLE_FILE_INFORMATION identity{};bool same=Ordinary(h,false,&identity)&&SameFile(stageIdentity,identity)&&identity.nFileSizeHigh==0&&identity.nFileSizeLow==trusted.size;
            std::size_t offset=0;const auto original=capture->Bytes();
            while(same&&offset<original.size()){DWORD read{};const auto wanted=DWORD(std::min<std::size_t>(transfer.size(),original.size()-offset));same=ReadFile(h,transfer.data(),wanted,&read,nullptr)&&read==wanted&&std::equal(transfer.begin(),transfer.begin()+read,original.begin()+offset);offset+=read;}
            FILE_DISPOSITION_INFO disposition{TRUE};same=same&&SetFileInformationByHandle(h,FileDispositionInfo,&disposition,sizeof(disposition));const bool closed=CloseHandle(h)!=FALSE;if(same&&closed){stageDeleted=true;return true;}return false;
        }catch(...){return false;}
    }
    /** @summary Observe real last-callback return, never equate HANDLE_CLOSING or5000ms with context disposal. */
    bool Settled() noexcept {
        if(!closing||closeFault)return false;
        if(unloadConfigured&&WaitForSingleObject(unloadEvent,0)!=WAIT_OBJECT_0)return false;
        if(!CleanupStage())return false;
        if(Stopped()){capture.reset();networkDone=false;if(error==BaseDependencyError::None)error=BaseDependencyError::Cancelled;}
        CloseConsole();terminal=true;
        if(error==BaseDependencyError::None&&networkDone){state=BaseDependencyState::Captured;capturePublished=true;}
        else if(error==BaseDependencyError::Cancelled)state=BaseDependencyState::Cancelled;
        else if(error==BaseDependencyError::Deadline)state=BaseDependencyState::Expired;
        else state=BaseDependencyState::Refused;
        return true;
    }
    static_assert((PathLimit+1)*sizeof(wchar_t)+sizeof(prompt)+sizeof(events)+sizeof(parents)<131072,"Input storage fits reviewed console envelope; path ownership is moved to Capture without a second full input string");
    static_assert(sizeof(Pin)*65+sizeof(map)+sizeof(urls)+sizeof(headers)+4*(StagingPathLimit+80)*sizeof(wchar_t)<262144,"Network controls plus conservative staging path temporaries fit262144; transfer is separately bounded");
};

BaseDependency::BaseDependency(logging::AsyncLogger& logger) noexcept:logger_(logger),thread_(GetCurrentThreadId()){}
BaseDependency::~BaseDependency(){
    RequestCancel();
    // @summary Original RuntimeContext must invoke/destruct on this ordinary owner thread offguards; no forced timeout disposal.
    while(body_&&!body_->terminal){(void)Pump();}
    while(body_&&body_->readers.load(std::memory_order_acquire))Sleep(100);
    body_.reset();
}
bool BaseDependency::Begin(const BaseDependencySelection& selected,const std::filesystem::path& stagingParent) noexcept {
    // @summary An invalid foreign entry must not write the original owner's nonatomic diagnostic state.
    if(GetCurrentThreadId()!=thread_)return false;
    if(body_){RequestCancel();lastError_=BaseDependencyError::Busy;return false;}
    if(generation_==std::numeric_limits<std::uint64_t>::max()){lastError_=BaseDependencyError::InvalidSelection;return false;}
    const auto epoch=cancellationEpoch_.load(std::memory_order_acquire);
    if(epoch==std::numeric_limits<std::uint64_t>::max()){lastError_=BaseDependencyError::Cancelled;return false;}
    try{auto original=std::make_unique<Body>();original->ownerEpoch=&cancellationEpoch_;original->admissionEpoch=epoch;if(!original->Select(selected,generation_+1)){lastError_=BaseDependencyError::InvalidSelection;return false;}
#if defined(SIMODER_BASE_DEPENDENCY_TEST)
        if(beginCut_)beginCut_(*this,BeginCut::Selected);
#endif
        if(original->Stopped()){lastError_=BaseDependencyError::Cancelled;return false;}
        // @summary Parents are retained before input; even a manual attempt belongs to the same bounded original slot.
        if(!original->Parents(stagingParent)){lastError_=BaseDependencyError::Stage;return false;}
#if defined(SIMODER_BASE_DEPENDENCY_TEST)
        if(beginCut_)beginCut_(*this,BeginCut::ParentsRetained);
#endif
        if(original->Stopped()){lastError_=BaseDependencyError::Cancelled;return false;}
        ++generation_;body_=std::move(original);lastError_=BaseDependencyError::None;
        if(body_->Stopped()){body_->Fail(BaseDependencyError::Cancelled);return false;}
        if(!body_->Console(logger_)){body_->Fail(body_->Stopped()?BaseDependencyError::Cancelled:BaseDependencyError::Console);return false;}return true;
    }catch(...){lastError_=BaseDependencyError::InvalidSelection;return false;}
}
void BaseDependency::RequestCancel() noexcept {auto observed=cancellationEpoch_.load(std::memory_order_relaxed);while(observed!=std::numeric_limits<std::uint64_t>::max()&&!cancellationEpoch_.compare_exchange_weak(observed,observed+1,std::memory_order_release,std::memory_order_relaxed)){} }
BaseDependencyState BaseDependency::Pump() noexcept {
    if(GetCurrentThreadId()!=thread_)return BaseDependencyState::Unresolved;
    if(!body_)return BaseDependencyState::Vacant;auto& b=*body_;
    if(b.Stopped())b.cancelled.store(true);
    if(b.terminal)return b.state;
    if(b.cancelled.load()&&!b.closing)b.Fail(BaseDependencyError::Cancelled);
    if(!b.closing){
        if(b.state==BaseDependencyState::Decision||b.state==BaseDependencyState::ManualPath){if(GetTickCount64()-b.decisionStart>=DecisionMs)b.Fail(BaseDependencyError::Deadline);else(void)b.Input();}
        else if(b.state==BaseDependencyState::Connecting){if(GetTickCount64()-b.acquireStart>=AcquisitionMs)b.Fail(BaseDependencyError::Deadline);else{bool closed{};{std::lock_guard lock(b.callbackMutex);closed=b.requestClosed[b.requestIndex];}if(closed)b.Request(b.requestIndex+1,b.nextUrlIndex);else Sleep(100);}}
        else b.Network();
    }
    if(b.closing&&!b.Settled()){
        if(GetTickCount64()-b.closeStart>=CloseObservationMs){b.state=BaseDependencyState::Unresolved;if(b.error==BaseDependencyError::None)b.error=BaseDependencyError::Cleanup;}
        Sleep(100);
    }
    lastError_=b.error;return b.state;
}
BaseDependency::CaptureReader BaseDependency::Capture() const noexcept {return GetCurrentThreadId()==thread_&&body_&&!body_->Stopped()&&body_->capture&&body_->capturePublished&&body_->readers.load()<65535?CaptureReader(body_.get()):CaptureReader{};}
bool BaseDependency::Release() noexcept {if(GetCurrentThreadId()!=thread_)return false;if(!body_)return true;if(!body_->terminal||body_->readers.load(std::memory_order_acquire))return false;body_.reset();return true;}
BaseDependency::CaptureReader::CaptureReader(Body* body) noexcept:body_(body){if(body_)body_->readers.fetch_add(1,std::memory_order_relaxed);}
BaseDependency::CaptureReader::~CaptureReader(){if(body_)body_->readers.fetch_sub(1,std::memory_order_release);}
BaseDependency::CaptureReader::CaptureReader(CaptureReader&& other) noexcept:body_(other.body_){other.body_=nullptr;}
BaseDependency::CaptureReader& BaseDependency::CaptureReader::operator=(CaptureReader&& other) noexcept {if(this!=&other){if(body_)body_->readers.fetch_sub(1,std::memory_order_release);body_=other.body_;other.body_=nullptr;}return *this;}
const Snapshot* BaseDependency::CaptureReader::Get() const noexcept{return body_?body_->capture.get():nullptr;}
std::uint64_t BaseDependency::Generation() const noexcept{return GetCurrentThreadId()==thread_?generation_:0;}
BaseDependencyError BaseDependency::Error() const noexcept{return GetCurrentThreadId()==thread_?lastError_:BaseDependencyError::Busy;}
}

namespace sc13::bootstrap {
/** @summary Borrow original immutable shared capture while the controller reader remains retained. */
std::shared_ptr<const platform::base_artifact::Snapshot> BaseDependency::CaptureReader::Share()const noexcept{return body_?body_->capture:nullptr;}
}
