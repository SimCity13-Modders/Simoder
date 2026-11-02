/** @file @summary One complete retained-file streaming CNG read with no allocation, path reopen or close-status masking. */
#include "platform/component_store/streaming_digest.hpp"
namespace sc13::platform::component_store {
DigestResult ReadRetainedDigest(StoreIO& io,File& file,const Observation& observed,
    std::uint64_t expected,core::Sha256Digest& digest)noexcept{
    if(observed.bytes!=expected)return {DigestStatus::SizeMismatch,{Status::Ok}};
    struct Reader final {StoreIO& io;File& file;Result last{Status::Ok};bool shortRead{};};
    Reader reader{io,file,{Status::Ok},false};
    const auto read=[](void* opaque,std::span<std::byte> output,std::size_t& received)noexcept{
        auto& current=*static_cast<Reader*>(opaque);current.last=current.io.Read(current.file,output,received);
        if(current.last.status==Status::Ok&&received!=output.size())current.shortRead=true;
        return current.last.status==Status::Ok;
    };
    std::array<std::byte,65536> input{},workspace{};core::Sha256Digest candidate{};
    std::size_t emptyCount{};
    const bool empty=expected!=0||(read(&reader,std::span<std::byte>(input.data(),0),emptyCount)&&emptyCount==0);
    const bool hashed=empty&&core::Sha256Read(read,&reader,expected,input,workspace,candidate);
    if(reader.last.status!=Status::Ok)return {DigestStatus::ReadFailure,reader.last};
    if(reader.shortRead)return {DigestStatus::ShortRead,reader.last};
    if(!hashed)return {DigestStatus::HashFailure,reader.last};
    digest=candidate;return {DigestStatus::Complete,reader.last};
}
}
