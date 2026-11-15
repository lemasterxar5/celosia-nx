#include "native_assets.hpp"
#include "native_files.hpp"
#include "native_memory.hpp"
#include "native_scus_assets.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <vector>
using namespace celosia::native;
namespace fs=std::filesystem;
static unsigned checks=0;
static void require(bool ok,const char *why) { ++checks; if(!ok) throw std::runtime_error(why); }
struct Fixture {
    fs::path base;
    std::vector<fs::path> owned;
    std::vector<uint8_t> a,b;
    Fixture() : a(2050),b(4096)
    {
        const auto nonce=std::chrono::steady_clock::now().time_since_epoch().count();
        for(unsigned i=0;i<100;++i) {
            base=fs::temp_directory_path()/("celosia-native-assets-"+std::to_string(nonce)+"-"+std::to_string(i));
            if(fs::create_directory(base)) { owned.push_back(base); break; }
        }
        if(owned.empty()) throw std::runtime_error("cannot create exclusive asset fixture");
        fs::create_directory(base/"DIR"); owned.push_back(base/"DIR");
        for(size_t i=0;i<a.size();++i) a[i]=static_cast<uint8_t>(i%251);
        for(size_t i=0;i<b.size();++i) b[i]=static_cast<uint8_t>(0xC0+i%32);
        file("A.BIN",a); file("DIR/B.BIN",b);
    }
    void file(const char *name,std::span<const uint8_t> bytes)
    {
        const auto path=base/name;
        if(!fs::exists(path)) owned.push_back(path);
        std::ofstream output(path,std::ios::binary|std::ios::trunc);
        output.write(reinterpret_cast<const char *>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
        if(!output) throw std::runtime_error("fixture write failed");
    }
    ~Fixture()
    { for(auto i=owned.rbegin();i!=owned.rend();++i) { std::error_code ignored; fs::remove(*i,ignored); } }
};
int main(int argc,char **argv)
{
    if(argc>2) throw std::invalid_argument("optional argument is the private extracted SCUS asset root");
    Fixture fixture;
    NativeFiles files(fixture.base.generic_string());
    DataMemory memory;
    const std::array<AssetEntry,2> entries{{{"A.BIN",4096,2050},{"DIR/B.BIN",16384,4096}}};
    NativeAssets assets(files,memory,entries);
    require(!assets.ready() && !assets.search("A.BIN"),"search requires verified initialization");
    require(!assets.readAtDataUnits(2,1,0),"read requires verified initialization");
    require(assets.lastError().has_value(),"not-ready error reported");
    require(assets.initialize() && assets.ready() && !assets.lastError(),"all configured native files verified");
    require(assets.initialize(),"initialization idempotent");
    const auto found=assets.search("cdrom0:\\dir\\b.bin;1");
    require(found && found->name=="DIR/B.BIN" && found->offset==16384 && found->bytes==4096,"search retains known archive metadata");
    require(!assets.search("UNKNOWN.BIN") && assets.lastError(),"unknown path is not fabricated");
    require(!assets.search("../A.BIN") && !assets.search("host:A.BIN"),"search cannot escape rooted files");
    require(assets.search(files.translate("A.BIN")).has_value() && !assets.lastError(),"rooted translation accepted and error clears");
    auto data=memory.mainRam();
    std::fill(data.begin()+0xFFFF,data.begin()+0x11001,0xA5);
    require(assets.readAtDataUnits(2,2,0x10000),"known two-block native asset read");
    require(std::equal(fixture.a.begin(),fixture.a.end(),data.begin()+0x10000),"actual file bytes copied");
    require(std::all_of(data.begin()+0x10000+2050,data.begin()+0x11000,[](uint8_t v){return v==0;}),"only final known block padded");
    require(data[0xFFFF]==0xA5 && data[0x11000]==0xA5,"read does not cross destination bounds");
    require(assets.readAtDataUnits(3,1,0x12000) && data[0x12000]==fixture.a[2048] && data[0x12001]==fixture.a[2049] && data[0x12002]==0,"last block native data and padding");
    require(assets.readAtDataUnits(9,1,0x13000) && std::equal(fixture.b.begin()+2048,fixture.b.end(),data.begin()+0x13000),"positioned read inside another native file");
    const auto untouched=[&] { return std::all_of(data.begin()+0x20000,data.begin()+0x22000,[](uint8_t v){return v==0xCD;}); };
    std::fill(data.begin()+0x20000,data.begin()+0x22000,0xCD);
    for(const auto request:std::array<std::array<uint32_t,3>,7>{{
        {4,1,0x20000},{2,3,0x20000},{2,0,0x20000},{2,UINT32_MAX,0x20000},
        {UINT32_MAX,1,0x20000},{2,1,DataMemory::MainBytes-2047},{2,1,0x70000000}}})
        require(!assets.readAtDataUnits(request[0],request[1],request[2]) && untouched(),"invalid complete request never mutates destination");
    for(const auto alias:{0u,0x20000000u,0x80000000u,0xA0000000u})
        require(assets.readAtDataUnits(2,1,alias+0x24000) && data[0x24000]==fixture.a[0],"bounded main data aliases supported");
    require(!assets.readAtDataUnits(2,1,0x11004000) && !assets.readAtDataUnits(2,1,0x12000000),"matrix buffers and hardware addresses rejected");
    require(assets.readAtDataUnits(2,1,0) && data[0]==fixture.a[0],"main data base is valid storage");
    require(assets.readAtDataUnits(8,2,DataMemory::MainBytes-4096) &&
            std::equal(fixture.b.begin(),fixture.b.end(),data.end()-4096),"read can exactly reach the main data end");
    auto invalid=[&](std::span<const AssetEntry> metadata) {
        NativeAssets bad(files,memory,metadata);
        require(!bad.initialize() && !bad.ready() && bad.lastError(),"invalid metadata fails closed");
    };
    invalid({});
    for(const auto bad:std::array<AssetEntry,7>{{
        {"../A.BIN",4096,2050},{"cdrom0:\\A.BIN",4096,2050},{"/A.BIN",4096,2050},
        {"A.BIN",4097,2050},{"A.BIN",UINT64_MAX-2047,4096},
        {"A.BIN",4096,UINT64_MAX},{std::string_view("A\0.BIN",6),4096,2050}}}) invalid(std::span(&bad,1));
    const std::array<AssetEntry,2> duplicate{{{"A.BIN",4096,2050},{"a.bin",16384,2050}}}; invalid(duplicate);
    const std::array<AssetEntry,2> overlap{{{"A.BIN",4096,2050},{"DIR/B.BIN",6144,4096}}}; invalid(overlap);
    const std::array<AssetEntry,1> wrongSize{{{"A.BIN",4096,2049}}}; invalid(wrongSize);
    std::vector<AssetEntry> excessive(NativeAssets::MaxEntries+1,entries[0]); invalid(excessive);
    {
        std::string name="A.BIN";
        const AssetEntry temporary{name,4096,2050};
        NativeAssets copied(files,memory,std::span(&temporary,1));
        name="../BROKEN";
        require(copied.initialize() && copied.search("A.BIN").has_value(),"metadata names are owned rather than borrowed");
    }
    {
        fixture.file("EMPTY",{});
        const AssetEntry empty{"EMPTY",32768,0};
        NativeAssets zero(files,memory,std::span(&empty,1));
        require(zero.initialize() && zero.search("EMPTY")->bytes==0 &&
                !zero.readAtDataUnits(16,1,0x20000) && untouched(),"empty native files never fabricate a data extent");
    }
    {
        const std::array<AssetEntry,2> adjacent{{{"A.BIN",4096,2050},{"DIR/B.BIN",8192,4096}}};
        NativeAssets exact(files,memory,adjacent);
        require(exact.initialize() && !exact.readAtDataUnits(3,2,0x20000) && untouched(),"read cannot synthesize adjacent-file ranges");
    }
    {
        const std::array<AssetEntry,2> retryEntries{{entries[0],{"RETRY.BIN",32768,1}}};
        NativeAssets retry(files,memory,retryEntries);
        require(!retry.initialize() && !retry.ready(),"missing native asset prevents readiness");
        const std::array<uint8_t,1> value{99}; fixture.file("RETRY.BIN",value);
        require(retry.initialize() && retry.readAtDataUnits(16,1,0x30000) && data[0x30000]==99 && data[0x30001]==0,"failed initialization can retry after real file appears");
    }
    const std::array<uint8_t,1> truncated{1}; fixture.file("A.BIN",truncated);
    require(!assets.readAtDataUnits(2,1,0x20000) && untouched(),"changed file size detected before destination mutation");
    if(argc==2) {
        NativeFiles realFiles(argv[1]);
        NativeAssets realAssets(realFiles,memory,ScusAssets);
        const bool initialized=realAssets.initialize();
        if(!initialized) {
            std::fprintf(stderr,"SCUS initialization: %s\n",realAssets.lastError()->c_str());
            for(const auto &entry:ScusAssets) {
                const auto token=realFiles.open(entry.name);
                if(!token || realFiles.size(token)!=entry.bytes)
                    std::fprintf(stderr,"SCUS native file unavailable/size mismatch: %.*s\n",int(entry.name.size()),entry.name.data());
                if(token) realFiles.close(token);
            }
        }
        require(initialized && realAssets.search("NICO.DAT")->bytes==2667340678ull,
                "private SCUS metadata and all native file sizes verified without reading asset bytes");
    }
    std::printf("native assets: %u checks passed\n",checks);
}
