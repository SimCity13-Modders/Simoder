#include "core/sha256.hpp"
#include <Windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <vector>
namespace sc13::core {
namespace {
/** @summary Preserve noexcept hashing failure even when allocating diagnostic text itself fails.
 * @param error Optional existing diagnostic destination. @param text Fixed nonsecret reason. */
void Error(std::string* error,const char* text) noexcept {if(error)try{*error=text;}catch(...){}}
/** @summary Own one actual CNG provider through the complete hash object's lifetime. */
class AlgorithmHandle final {
public:
    /** @summary Construct an inert handle. */
    AlgorithmHandle() noexcept=default;
    /** @summary Release only the original successfully opened provider. */
    ~AlgorithmHandle(){if(value_)BCryptCloseAlgorithmProvider(value_,0);}
    AlgorithmHandle(const AlgorithmHandle&)=delete;
    AlgorithmHandle& operator=(const AlgorithmHandle&)=delete;
    /** @summary Borrow original open destination. @return Handle destination. */
    BCRYPT_ALG_HANDLE* put() noexcept{return &value_;}
    /** @summary Borrow retained original provider. @return Actual handle. */
    BCRYPT_ALG_HANDLE get() const noexcept{return value_;}
private:BCRYPT_ALG_HANDLE value_{};
};
/** @summary Own the original CNG hash object; object workspace is destroyed only afterward. */
class HashHandle final {
public:
    /** @summary Construct an inert hash. */
    HashHandle() noexcept=default;
    /** @summary Destroy the actual hash before its earlier-declared object workspace and provider. */
    ~HashHandle(){if(value_)BCryptDestroyHash(value_);}
    HashHandle(const HashHandle&)=delete;
    HashHandle& operator=(const HashHandle&)=delete;
    /** @summary Borrow original create destination. @return Handle destination. */
    BCRYPT_HASH_HANDLE* put() noexcept{return &value_;}
    /** @summary Borrow actual original hash. @return Actual handle. */
    BCRYPT_HASH_HANDLE get() const noexcept{return value_;}
private:BCRYPT_HASH_HANDLE value_{};
};
/** @summary Update the same original CNG hash over bounded native chunks, shared by all byte/file callers.
 * @param hash Original retained hash. @param data Exact borrowed bytes. @param error Optional diagnostic.
 * @return False on a factual update error, without allocating output storage. */
bool Append(BCRYPT_HASH_HANDLE hash,std::span<const std::byte> data,std::string* error) noexcept {
    constexpr auto maximum=static_cast<std::size_t>((std::numeric_limits<ULONG>::max)());
    std::size_t offset{};
    while(offset<data.size()){
        const auto count=(std::min)(maximum,data.size()-offset);
        if(BCryptHashData(hash,reinterpret_cast<PUCHAR>(const_cast<std::byte*>(data.data()+offset)),static_cast<ULONG>(count),0)<0){
            Error(error,"BCryptHashData failed");return false;
        }
        offset+=count;
    }
    return true;
}
/** @summary One canonical CNG hash construction/finalization; charged callers cannot fall back to owned storage.
 * @tparam Producer Existing bounded byte/file producer. @param producer Supplies original bytes once.
 * @param digest Complete output on success. @param error Optional nonsecret diagnostic.
 * @param external Original charged workspace. @param ownWorkspace True only for the existing noncharged utility entry.
 * @return Factual hash status; every native handle/object remains live through producer and finish. */
template<class Producer>
bool ComputeHash(Producer&& producer,Sha256Digest& digest,std::string* error,
    std::span<std::byte> external,bool ownWorkspace) noexcept {
    try{
        AlgorithmHandle algorithm;
        if(BCryptOpenAlgorithmProvider(algorithm.put(),BCRYPT_SHA256_ALGORITHM,nullptr,0)<0){
            Error(error,"BCryptOpenAlgorithmProvider failed");return false;
        }
        DWORD objectLength{},resultLength{};
        if(BCryptGetProperty(algorithm.get(),BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&objectLength),
            sizeof(objectLength),&resultLength,0)<0||resultLength!=sizeof(objectLength)||!objectLength){
            Error(error,"BCryptGetProperty(BCRYPT_OBJECT_LENGTH) failed");return false;
        }
        std::vector<UCHAR> owned;
        PUCHAR object{};
        if(ownWorkspace){owned.resize(objectLength);object=owned.data();}
        else{
            if(objectLength>external.size()||!external.data())return false;
            object=reinterpret_cast<PUCHAR>(external.data());
        }
        HashHandle hash;
        if(BCryptCreateHash(algorithm.get(),hash.put(),object,objectLength,nullptr,0,0)<0){
            Error(error,"BCryptCreateHash failed");return false;
        }
        if(!producer(hash.get(),error))return false;
        Sha256Digest completed{};
        if(BCryptFinishHash(hash.get(),reinterpret_cast<PUCHAR>(completed.data()),static_cast<ULONG>(completed.size()),0)<0){
            Error(error,"BCryptFinishHash failed");return false;
        }
        digest=completed;return true;
    }catch(...){Error(error,"SHA-256 computation failed due to an allocation or I/O exception");return false;}
}
}
bool Sha256(std::span<const std::byte> data,Sha256Digest& digest,std::string& error) noexcept {
    return ComputeHash([data](BCRYPT_HASH_HANDLE hash,std::string* reason) noexcept{return Append(hash,data,reason);},digest,&error,{},true);
}
bool Sha256(std::span<const std::byte> data,std::span<std::byte> workspace,Sha256Digest& digest) noexcept {
    if(workspace.empty())return false;
    return ComputeHash([data](BCRYPT_HASH_HANDLE hash,std::string* reason) noexcept{return Append(hash,data,reason);},digest,nullptr,workspace,false);
}
bool Sha256(std::initializer_list<std::span<const std::byte>> parts,std::span<std::byte> workspace,Sha256Digest& digest) noexcept {
    if(workspace.empty())return false;
    return ComputeHash([parts](BCRYPT_HASH_HANDLE hash,std::string* reason) noexcept{
        for(const auto part:parts)if(!Append(hash,part,reason))return false;return true;
    },digest,nullptr,workspace,false);
}
/** @summary Detect physical overlap with checked ranges before invoking any original reader. */
static bool HashDisjoint(const void* left,std::size_t leftBytes,const void* right,std::size_t rightBytes)noexcept{
    const auto a=reinterpret_cast<std::uintptr_t>(left),b=reinterpret_cast<std::uintptr_t>(right);
    if(leftBytes>(std::numeric_limits<std::uintptr_t>::max)()-a||rightBytes>(std::numeric_limits<std::uintptr_t>::max)()-b)return false;
    return a+leftBytes<=b||b+rightBytes<=a;
}
bool Sha256Read(Sha256ReadCallback reader,void* context,std::uint64_t expectedBytes,std::span<std::byte> input,
    std::span<std::byte> workspace,Sha256Digest& digest)noexcept{
    if(!reader||input.empty()||workspace.empty()||!input.data()||!workspace.data()||
        !HashDisjoint(input.data(),input.size(),workspace.data(),workspace.size())||
        !HashDisjoint(input.data(),input.size(),digest.data(),digest.size())||
        !HashDisjoint(workspace.data(),workspace.size(),digest.data(),digest.size()))return false;
    return ComputeHash([&](BCRYPT_HASH_HANDLE hash,std::string* reason)noexcept{
        std::uint64_t offset{};
        while(offset<expectedBytes){const auto count=static_cast<std::size_t>((std::min<std::uint64_t>)(input.size(),expectedBytes-offset));
            std::size_t received{};if(!reader(context,input.first(count),received)||received!=count)return false;
            if(!Append(hash,std::span<const std::byte>(input.data(),received),reason))return false;offset+=received;
        }
        return true;
    },digest,nullptr,workspace,false);
}
bool Sha256File(const std::filesystem::path& path,Sha256Digest& digest,std::string& error) noexcept {
    return ComputeHash([&path](BCRYPT_HASH_HANDLE hash,std::string* reason){
        std::ifstream stream(path,std::ios::binary);
        if(!stream){Error(reason,"Could not open file for SHA-256");return false;}
        std::array<char,64*1024> buffer{};
        while(stream){
            stream.read(buffer.data(),static_cast<std::streamsize>(buffer.size()));
            const auto read=stream.gcount();
            if(read>0&&!Append(hash,std::as_bytes(std::span(buffer.data(),static_cast<std::size_t>(read))),reason))return false;
        }
        if(!stream.eof()){Error(reason,"I/O error while hashing file");return false;}
        return true;
    },digest,&error,{},true);
}
std::string ToHex(const Sha256Digest& digest){
    constexpr char digits[]="0123456789ABCDEF";std::string result(digest.size()*2,'0');
    for(std::size_t index=0;index<digest.size();++index){const auto value=static_cast<unsigned char>(digest[index]);
        result[index*2]=digits[value>>4U];result[index*2+1]=digits[value&0x0fU];}
    return result;
}
}
