#include "owned_backend.hpp"
#include "thread_placement.hpp"
#include "native_executor.hpp"
#include "../../../src/lib/native/native_backend.h"
#include "../../../src/lib/native/deko3d/policy.hpp"
#include "../../../src/lib/native/deko3d/render_progress.hpp"
#include <array>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <unordered_set>
#include <utility>
namespace native_renderer::detail {
namespace {
// v122: a POD array that keeps its storage between frames and never zero-fills (std::vector::resize
// value-initialises every element).
template<class T> class PodArray {
    std::unique_ptr<T[]> items;size_t used=0,room=0;
public:
    T *grow(size_t count)
    {
        if(count>room-used){
            size_t next=room?room:4096;while(next-used<count)next*=2;
            std::unique_ptr<T[]> bigger(new T[next]);
            if(used)std::memcpy(static_cast<void *>(bigger.get()),items.get(),used*sizeof(T));
            items=std::move(bigger);room=next;
        }
        T *at=items.get()+used;used+=count;return at;
    }
    void shrink(size_t to){used=to;}
    size_t size()const{return used;}
    const T *data()const{return items.get();}
    T *data(){return items.get();}
    void clear(){used=0;}
};
// v122: a frame's commands as typed records in storage kept between frames (two batches: the caller fills
// one while the render worker runs the other). Before: one std::function per command, a heap block per
// draw (its DrawState and matrix) and two copies of every per-frame mesh into fresh vectors, allocated on
// the game thread and freed on the render worker (newlib's malloc has one lock). Same commands, same
// order, same data.
struct FrameBatch {
    enum Kind:uint8_t{Op,DrawCall,FrameMeshCall};
    struct Entry{Kind kind;uint32_t index;};
    struct DrawRecord{uint64_t mesh;std::array<float,16> clip;DrawState state;};
    struct MeshRecord{uint64_t id;uint32_t firstVertex,count,firstIndex,indexCount;};
    std::vector<Entry> order;
    std::vector<std::function<void(Backend &)>> ops;
    std::vector<DrawRecord> draws;
    std::vector<MeshRecord> meshes;
    PodArray<Vertex> vertices;
    PodArray<uint32_t> indices;
    void clear(){order.clear();ops.clear();draws.clear();meshes.clear();vertices.clear();indices.clear();}
    void run(Backend &backend)const
    {
        for(const auto &entry:order){
            if(entry.kind==Op){ops[entry.index](backend);continue;}
            if(entry.kind==DrawCall){const auto &d=draws[entry.index];backend.draw(d.mesh,d.clip.data(),d.state);continue;}
            const auto &m=meshes[entry.index];
            auto *destination=backend.frameMesh(m.count);
            if(!destination)throw std::runtime_error("Native GPU frame mesh unavailable");
            std::memcpy(static_cast<void *>(destination),vertices.data()+m.firstVertex,size_t(m.count)*sizeof(Vertex));
            try {
                if(m.indexCount) {
                    uint32_t base=0;
                    auto *target=backend.frameMeshIndices(m.indexCount,base);
                    if(!target || base>UINT32_MAX-(m.count-1))
                        throw std::runtime_error("Native GPU index range unavailable");
                    const uint32_t *source=indices.data()+m.firstIndex;
                    for(uint32_t i=0;i<m.indexCount;++i)target[i]=source[i]+base;
                }
                backend.commitFrameMesh(m.id,m.count,m.indexCount);
            }catch(...){backend.cancelFrameMesh();throw;}
        }
    }
};
class OwnedBackend final:public Backend {
    celosia::native::NativeExecutor worker;
    std::unique_ptr<Backend> gpu;
    std::mutex mutex;
    FrameBatch batches[2];
    FrameBatch *filling=&batches[0];
    std::unordered_set<uint64_t> meshes,textures,transientMeshes;
    size_t pendingVertexBase=0,pendingIndexBase=0;
    uint32_t pendingVertices=0,pendingIndices=0;
    size_t queuedBytes=0;
    bool open=false,failed=false;
    void ready()const{if(failed)throw std::runtime_error("Native GPU owner has failed");}
    void room(size_t bytes)
    {
        ready();if(filling->order.size()>=65536 || bytes>256u*1024u*1024u-queuedBytes)
            throw std::runtime_error("Native GPU frame batch capacity exceeded");
    }
    void enqueue(std::function<void(Backend &)> operation,size_t bytes=0)
    {
        room(bytes);
        filling->order.push_back({FrameBatch::Op,uint32_t(filling->ops.size())});
        filling->ops.push_back(std::move(operation));queuedBytes+=bytes;
    }
    // CELOSIA_DIAG_SYNC_RENDER=1: the caller waits for each frame's recording (previous behaviour).
    const bool asyncFrames=[]{const char *sync=std::getenv("CELOSIA_DIAG_SYNC_RENDER");return !(sync && std::atoi(sync)!=0);}();
    // asynchronous: the frame's batch (it owns all its data) records on the render worker while the
    // caller builds the next frame; any later call waits for it first (one frame in flight at most).
    // The worker runs one batch at a time and execute/post first wait for the job before: when post
    // returns, the other batch is idle and becomes the one filled next.
    void flush(bool asynchronous=false)
    {
        FrameBatch *batch=nullptr;
        {std::lock_guard lock(mutex);ready();batch=filling;queuedBytes=0;}
        deko::markRender(deko::RenderPhase::Dispatch);
        try{
            auto job=[this,batch]{batch->run(*gpu);};
            if(asynchronous)worker.post(std::move(job));
            else {worker.execute(std::move(job));deko::markRender(deko::RenderPhase::Idle);}
        }
        catch(...){deko::markRender(deko::RenderPhase::Failed);std::lock_guard lock(mutex);failed=true;throw;}
        std::lock_guard lock(mutex);
        filling=batch==&batches[0]?&batches[1]:&batches[0];
        filling->clear();
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
    // The caller writes the mesh straight into the batch (no staging copies).
    Vertex *frameMesh(uint32_t count)override
    {
        deko::vertexBytes(count);std::lock_guard lock(mutex);ready();
        if(pendingVertices){filling->vertices.shrink(pendingVertexBase);filling->indices.shrink(pendingIndexBase);}
        pendingVertexBase=filling->vertices.size();pendingIndexBase=filling->indices.size();
        Vertex *at=filling->vertices.grow(count);pendingVertices=count;pendingIndices=0;
        return at;
    }
    uint32_t *frameMeshIndices(uint32_t count,uint32_t &base)override
    {
        std::lock_guard lock(mutex);ready();
        if(!pendingVertices || !count || count%3 || count>16u*1024u*1024u)
            throw std::invalid_argument("Invalid pending native index list");
        filling->indices.shrink(pendingIndexBase);
        uint32_t *at=filling->indices.grow(count);pendingIndices=count;base=0;return at;
    }
    void commitFrameMesh(uint64_t id,uint32_t count,uint32_t indexCount)override
    {
        std::lock_guard lock(mutex);ready();
        if(!id || !count || count>pendingVertices || indexCount>pendingIndices)
            throw std::invalid_argument("Invalid pending native mesh range");
        const auto bytes=indexCount?deko::vertexBytes(count):deko::meshBytes(count);
        if(indexCount)deko::validateIndices(filling->indices.data()+pendingIndexBase,indexCount,count);
        const auto totalBytes=bytes+size_t(indexCount)*sizeof(uint32_t);
        room(totalBytes);
        filling->vertices.shrink(pendingVertexBase+count);filling->indices.shrink(pendingIndexBase+indexCount);
        filling->order.push_back({FrameBatch::FrameMeshCall,uint32_t(filling->meshes.size())});
        filling->meshes.push_back({id,uint32_t(pendingVertexBase),count,uint32_t(pendingIndexBase),indexCount});
        queuedBytes+=totalBytes;
        meshes.insert(id);transientMeshes.insert(id);pendingVertices=pendingIndices=0;
    }
    void cancelFrameMesh()override
    {
        std::lock_guard lock(mutex);ready();
        if(pendingVertices){filling->vertices.shrink(pendingVertexBase);filling->indices.shrink(pendingIndexBase);}
        pendingVertices=pendingIndices=0;
    }
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
        room(0);
        filling->order.push_back({FrameBatch::DrawCall,uint32_t(filling->draws.size())});
        filling->draws.push_back({mesh,matrix,state});
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
