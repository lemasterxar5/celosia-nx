#include "owned_backend.hpp"
#include "thread_placement.hpp"
#include "native_executor.hpp"
#include "../../../src/lib/native/native_backend.h"
#include "../../../src/lib/native/deko3d/policy.hpp"
#include "../../../src/lib/native/deko3d/render_progress.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <unordered_set>
#include <utility>
namespace native_renderer::detail {
namespace {
class OwnedBackend final:public Backend {
    celosia::native::NativeExecutor worker;
    std::unique_ptr<Backend> gpu;
    std::mutex mutex;
    std::vector<std::function<void(Backend &)>> commands;
    std::unordered_set<uint64_t> meshes,textures,transientMeshes;
    std::vector<Vertex> frameVertices;
    std::vector<uint32_t> frameIndices;
    uint32_t pendingVertices=0,pendingIndices=0;
    size_t queuedBytes=0;
    bool open=false,failed=false;
    void ready()const{if(failed)throw std::runtime_error("Native GPU owner has failed");}
    void enqueue(std::function<void(Backend &)> operation,size_t bytes=0)
    {
        ready();if(commands.size()>=65536 || bytes>256u*1024u*1024u-queuedBytes)
            throw std::runtime_error("Native GPU frame batch capacity exceeded");
        commands.push_back(std::move(operation));queuedBytes+=bytes;
    }
    // CELOSIA_DIAG_SYNC_RENDER=1: the caller waits for each frame's recording (previous behaviour).
    const bool asyncFrames=[]{const char *sync=std::getenv("CELOSIA_DIAG_SYNC_RENDER");return !(sync && std::atoi(sync)!=0);}();
    // asynchronous: the frame's batch (it owns all its data) records on the render worker while the
    // caller builds the next frame; any later call waits for it first (one frame in flight at most).
    void flush(bool asynchronous=false)
    {
        std::vector<std::function<void(Backend &)>> batch;
        {std::lock_guard lock(mutex);ready();batch=std::move(commands);commands.clear();queuedBytes=0;}
        deko::markRender(deko::RenderPhase::Dispatch);
        try{
            auto job=[this,batch=std::move(batch)]{for(const auto &operation:batch)operation(*gpu);};
            if(asynchronous)worker.post(std::move(job));
            else {worker.execute(std::move(job));deko::markRender(deko::RenderPhase::Idle);}
        }
        catch(...){deko::markRender(deko::RenderPhase::Failed);std::lock_guard lock(mutex);failed=true;throw;}
    }
public:
    explicit OwnedBackend(std::function<Backend *()> factory)
    {
        if(!factory)throw std::invalid_argument("Missing native GPU factory");
        worker.execute([&]{celosia::native::placeSwitchThread("render",1);gpu.reset(factory());if(!gpu)throw std::runtime_error("Native GPU factory failed");});
    }
    ~OwnedBackend()override
    {
        try{try{worker.drain();}catch(...){std::fputs("Native GPU frame failed before cleanup\n",stderr);}
            worker.execute([&]{gpu.reset();});}
        catch(...){std::fputs("Native GPU owner cleanup failed\n",stderr);std::terminate();}
    }
    bool hasMesh(uint64_t id)override{std::lock_guard lock(mutex);ready();return meshes.count(id)!=0;}
    bool hasTexture(uint64_t id)override{std::lock_guard lock(mutex);ready();return textures.count(id)!=0;}
    Vertex *frameMesh(uint32_t count)override
    {
        deko::vertexBytes(count);std::lock_guard lock(mutex);ready();
        frameVertices.resize(count);pendingVertices=count;pendingIndices=0;
        return frameVertices.data();
    }
    uint32_t *frameMeshIndices(uint32_t count,uint32_t &base)override
    {
        std::lock_guard lock(mutex);ready();
        if(!pendingVertices || !count || count%3 || count>16u*1024u*1024u)
            throw std::invalid_argument("Invalid pending native index list");
        frameIndices.resize(count);pendingIndices=count;base=0;return frameIndices.data();
    }
    void commitFrameMesh(uint64_t id,uint32_t count,uint32_t indexCount)override
    {
        std::lock_guard lock(mutex);ready();
        if(!id || !count || count>pendingVertices || indexCount>pendingIndices)
            throw std::invalid_argument("Invalid pending native mesh range");
        const auto bytes=indexCount?deko::vertexBytes(count):deko::meshBytes(count);
        if(indexCount)deko::validateIndices(frameIndices.data(),indexCount,count);
        const auto totalBytes=bytes+size_t(indexCount)*sizeof(uint32_t);
        if(commands.size()>=65536 || totalBytes>256u*1024u*1024u-queuedBytes)
            throw std::runtime_error("Native GPU frame batch capacity exceeded");
        std::vector<Vertex> vertices(frameVertices.begin(),frameVertices.begin()+count);
        std::vector<uint32_t> indices(frameIndices.begin(),frameIndices.begin()+indexCount);
        enqueue([id,vertices=std::move(vertices),indices=std::move(indices)](Backend &backend){
            auto *destination=backend.frameMesh(static_cast<uint32_t>(vertices.size()));
            if(!destination)throw std::runtime_error("Native GPU frame mesh unavailable");
            std::copy(vertices.begin(),vertices.end(),destination);
            try {
                if(!indices.empty()) {
                    uint32_t base=0;
                    auto *target=backend.frameMeshIndices(static_cast<uint32_t>(indices.size()),base);
                    if(!target || base>UINT32_MAX-(vertices.size()-1))
                        throw std::runtime_error("Native GPU index range unavailable");
                    for(size_t i=0;i<indices.size();++i)target[i]=indices[i]+base;
                }
                backend.commitFrameMesh(id,static_cast<uint32_t>(vertices.size()),static_cast<uint32_t>(indices.size()));
            }catch(...){backend.cancelFrameMesh();throw;}
        },totalBytes);
        meshes.insert(id);transientMeshes.insert(id);pendingVertices=pendingIndices=0;
    }
    void cancelFrameMesh()override
    {std::lock_guard lock(mutex);ready();pendingVertices=pendingIndices=0;}
    void uploadMesh(uint64_t id,const Vertex *source,uint32_t count,bool transient)override
    {
        const auto bytes=deko::meshBytes(count);if(!source || !id)throw std::invalid_argument("Invalid native mesh upload");
        std::vector<Vertex> vertices(source,source+count);std::lock_guard lock(mutex);
        enqueue([id,vertices=std::move(vertices),transient](Backend &backend){backend.uploadMesh(id,vertices.data(),vertices.size(),transient);},bytes);
        meshes.insert(id);
        if(transient)transientMeshes.insert(id);else transientMeshes.erase(id);
    }
    void uploadTexture(uint64_t id,uint32_t width,uint32_t height,const uint8_t *source,uint32_t levels)override
    {
        const auto bytes=deko::textureBytes(width,height,levels);if(!source || !id)throw std::invalid_argument("Invalid native image upload");
        std::vector<uint8_t> rgba(source,source+bytes);std::lock_guard lock(mutex);
        enqueue([id,width,height,levels,rgba=std::move(rgba)](Backend &backend){backend.uploadTexture(id,width,height,rgba.data(),levels);},bytes);
        textures.insert(id);
    }
    void beginFrame()override
    {
        deko::markRender(deko::RenderPhase::CpuFrame);
        std::lock_guard lock(mutex);ready();if(open)throw std::logic_error("Native GPU frame already open");
        enqueue([](Backend &backend){backend.beginFrame();});open=true;
    }
    void draw(uint64_t mesh,const float *clip,const DrawState &state)override
    {
        if(!clip)throw std::invalid_argument("Missing native draw transform");
        // setFog rejects enabled depth compositing before it can be queued.
        deko::validateDraw(state,true);std::array<float,16> matrix;std::copy_n(clip,16,matrix.begin());
        for(const auto value:matrix)if(!std::isfinite(value))throw std::invalid_argument("Invalid native draw transform");
        std::lock_guard lock(mutex);ready();
        if(!open || !meshes.count(mesh) || (state.texture&&!textures.count(state.texture)))throw std::logic_error("Native draw has missing resources/frame");
        enqueue([mesh,matrix,state](Backend &backend){backend.draw(mesh,matrix.data(),state);});
    }
    void setFog(const Fog &value)override
    {deko::validateFog(value);std::lock_guard lock(mutex);enqueue([value](Backend &backend){backend.setFog(value);});}
    void setBloom(const Bloom &value)override
    {std::lock_guard lock(mutex);enqueue([value](Backend &backend){backend.setBloom(value);});}
    void addScreenBlur(const ScreenBlur &value)override
    {std::lock_guard lock(mutex);enqueue([value](Backend &backend){backend.addScreenBlur(value);});}
    void setGlowComposite(const GlowComposite &value)override
    {std::lock_guard lock(mutex);enqueue([value](Backend &backend){backend.setGlowComposite(value);});}
    void setDepthOfField(const DepthOfField &value)override
    {std::lock_guard lock(mutex);enqueue([value](Backend &backend){backend.setDepthOfField(value);});}
    void holdFrame()override
    {std::lock_guard lock(mutex);enqueue([](Backend &backend){backend.holdFrame();});}
    void endFrame(uint32_t width,uint32_t height)override
    {
        if(width!=1280 || height!=720)throw std::invalid_argument("Native raster must be 1280x720");
        {std::lock_guard lock(mutex);ready();if(!open)throw std::logic_error("Native GPU frame is closed");
            enqueue([width,height](Backend &backend){backend.endFrame(width,height);});open=false;}
        flush(asyncFrames);
        // The GPU drops transient mesh ownership after submission. Mirror its
        // cache here so subsequent frames rebuild animated geometry correctly.
        std::lock_guard lock(mutex);
        for(const auto id:transientMeshes)meshes.erase(id);
        transientMeshes.clear();
    }
    void finishFrames()override
    {
        flush();worker.execute([&]{gpu->finishFrames();});
    }
    bool latestFrame(std::vector<uint8_t> &rgba,uint32_t &width,uint32_t &height)override
    {
        bool result=false;
        try{worker.execute([&]{result=gpu->latestFrame(rgba,width,height);});}
        catch(...){std::lock_guard lock(mutex);failed=true;throw;}
        return result;
    }
};
}
Backend *createOwnedDekoBackend(std::function<Backend *()> factory){return new OwnedBackend(std::move(factory));}
}
