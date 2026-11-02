#include "offload_roundtrip.hpp"

#include <cstdlib>

namespace {

void waitFence(DeviceContext* ctx, VkFence f, const char* what) {
    if (!f) return;
    vkCheck(ctx->vk.WaitForFences(ctx->device, 1, &f, VK_TRUE, UINT64_MAX), what);
}

void resetFence(DeviceContext* ctx, VkFence f, const char* what) {
    vkCheck(ctx->vk.ResetFences(ctx->device, 1, &f), what);
}

void beginOneShot(DeviceContext* ctx, VkCommandBuffer cmd) {
    vkCheck(ctx->vk.ResetCommandBuffer(cmd, 0), "rt reset cmd");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkCheck(ctx->vk.BeginCommandBuffer(cmd, &bi), "rt begin cmd");
}

void endSubmit(DeviceContext* ctx, VkCommandBuffer cmd, VkFence fence, const char* what,
               uint32_t waitCount = 0, const VkSemaphore* waits = nullptr,
               const VkPipelineStageFlags* stages = nullptr) {
    vkCheck(ctx->vk.EndCommandBuffer(cmd), "rt end cmd");
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    if (waitCount && waits) {
        si.waitSemaphoreCount = waitCount;
        si.pWaitSemaphores = waits;
        si.pWaitDstStageMask = stages;
    }
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkCheck(ctx->vk.QueueSubmit(ctx->graphicsQueue, 1, &si, fence), what);
}

} // namespace

bool OffloadRoundTrip::legacyRequested() {
    static const bool legacy = [] {
        const char* e = std::getenv("XFSR_RT_LEGACY");
        return e && e[0] == '1';
    }();
    return legacy;
}

const GpuImage& OffloadRoundTrip::submitCustom(FrameTransfer& fwd, FrameTransfer& back,
                                               RecordFn recordFn, void* user,
                                               const GpuImage& appImage, uint32_t waitSemCount,
                                               const VkSemaphore* waitSems,
                                               const VkPipelineStageFlags* waitStages,
                                               VkImageLayout appLayout) {
    constexpr int S = FrameTransfer::kSlots;
    DeviceContext* dg = fwd.srcCtx; // app / present GPU
    DeviceContext* ig = fwd.dstCtx; // FSR GPU
    dg->loadDispatch();
    ig->loadDispatch();

    // B — iGPU: one fused submit (upload → upscale → download). No host wait after.
    auto stageB = [&](int j) {
        FrameTransfer::Slot& fs = fwd.slots[j];
        FrameTransfer::Slot& bs = back.slots[j];
        if (!fs.pendingUpload) return;
        waitFence(dg, fs.srcFence, "rt B wait download");      // A(j) wrote fwd buf
        waitFence(dg, bs.dstFence, "rt B wait back reuse");    // prior C read back buf
        waitFence(ig, bs.srcFence, "rt B wait igpu reuse");    // prior B on this slot
        resetFence(ig, bs.srcFence, "rt B reset");
        beginOneShot(ig, bs.srcCmd);
        fwd.recordUpload(bs.srcCmd, j);
        const GpuImage& upOut = recordFn(user, bs.srcCmd, fs.dstOptimal);
        back.recordDownload(bs.srcCmd, upOut, j, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        endSubmit(ig, bs.srcCmd, bs.srcFence, "rt B submit");
        fs.pendingUpload = false;
        bs.pendingUpload = true;
    };

    // C — dGPU: back buf → display image. Same queue as the present blit that
    // follows, so queue order + barriers cover the dependency (no host wait).
    auto stageC = [&](int i) {
        FrameTransfer::Slot& bs = back.slots[i];
        if (!bs.pendingUpload) return;
        waitFence(ig, bs.srcFence, "rt C wait igpu");
        waitFence(dg, bs.dstFence, "rt C wait reuse");
        resetFence(dg, bs.dstFence, "rt C reset");
        beginOneShot(dg, bs.dstCmd);
        back.recordUpload(bs.dstCmd, i);
        endSubmit(dg, bs.dstCmd, bs.dstFence, "rt C submit");
        bs.pendingUpload = false;
        back.roundTripReady = i;
    };

    // A — dGPU: app image → fwd buf. Carries the app's present wait-semaphores.
    auto stageA = [&](int k) {
        FrameTransfer::Slot& fs = fwd.slots[k];
        FrameTransfer::Slot& bs = back.slots[k];
        if (fs.pendingUpload) stageB(k); // never in steady state; keeps flags consistent
        waitFence(dg, fs.srcFence, "rt A wait reuse");          // prior A on this slot
        waitFence(ig, bs.srcFence, "rt A wait igpu read");      // prior B read fwd buf
        resetFence(dg, fs.srcFence, "rt A reset");
        beginOneShot(dg, fs.srcCmd);
        fwd.recordDownload(fs.srcCmd, appImage, k, appLayout);
        endSubmit(dg, fs.srcCmd, fs.srcFence, "rt A submit", waitSemCount, waitSems, waitStages);
        fs.pendingUpload = true;
    };

    const int k = fwd.produceIndex % S;
    // B and C first: C must precede A on the dGPU queue, otherwise A's
    // ALL_COMMANDS barrier would hold the upload until the app frame finishes.
    stageB((k + S - 1) % S);
    stageC((k + S - 2) % S);
    stageA(k);

    if (back.roundTripReady < 0) {
        // Cold start (first frame after init/resize): drain synchronously so the
        // first present has pixels. Later frames repeat this until the pipe fills.
        stageB(k);
        stageC(k);
    }

    fwd.produceIndex = (k + 1) % S;
    if (fwd.framesProduced < (1 << 30)) ++fwd.framesProduced;
    return back.slots[back.roundTripReady].dstOptimal;
}

const GpuImage& OffloadRoundTrip::submit(FrameTransfer& fwd, FrameTransfer& back, FsrPass& fsr,
                                         const GpuImage& appImage, uint32_t waitSemCount,
                                         const VkSemaphore* waitSems,
                                         const VkPipelineStageFlags* waitStages,
                                         VkImageLayout appLayout) {
    auto fn = [](void* u, VkCommandBuffer cmd, const GpuImage& src) -> const GpuImage& {
        auto* p = static_cast<FsrPass*>(u);
        p->record(cmd, src);
        return p->output();
    };
    return submitCustom(fwd, back, fn, &fsr, appImage, waitSemCount, waitSems, waitStages,
                        appLayout);
}

const GpuImage& OffloadRoundTrip::submit(FrameTransfer& fwd, FrameTransfer& back, EdgePass& edge,
                                         const GpuImage& appImage, uint32_t waitSemCount,
                                         const VkSemaphore* waitSems,
                                         const VkPipelineStageFlags* waitStages,
                                         VkImageLayout appLayout) {
    auto fn = [](void* u, VkCommandBuffer cmd, const GpuImage& src) -> const GpuImage& {
        auto* p = static_cast<EdgePass*>(u);
        p->record(cmd, src);
        return p->output();
    };
    return submitCustom(fwd, back, fn, &edge, appImage, waitSemCount, waitSems, waitStages,
                        appLayout);
}

const GpuImage& OffloadRoundTrip::submit(FrameTransfer& fwd, FrameTransfer& back, NisPass& nis,
                                         const GpuImage& appImage, uint32_t waitSemCount,
                                         const VkSemaphore* waitSems,
                                         const VkPipelineStageFlags* waitStages,
                                         VkImageLayout appLayout) {
    auto fn = [](void* u, VkCommandBuffer cmd, const GpuImage& src) -> const GpuImage& {
        auto* p = static_cast<NisPass*>(u);
        p->record(cmd, src);
        return p->output();
    };
    return submitCustom(fwd, back, fn, &nis, appImage, waitSemCount, waitSems, waitStages,
                        appLayout);
}

const GpuImage& OffloadRoundTrip::submitBlit(FrameTransfer& fwd, FrameTransfer& back,
                                             EdgePass& edgeScratch, const GpuImage& appImage,
                                             uint32_t waitSemCount, const VkSemaphore* waitSems,
                                             const VkPipelineStageFlags* waitStages,
                                             VkImageLayout appLayout, VkFilter filter) {
    struct BlitUser {
        EdgePass* edge;
        VkFilter filter;
    };
    BlitUser user{&edgeScratch, filter};
    auto fn = [](void* u, VkCommandBuffer cmd, const GpuImage& src) -> const GpuImage& {
        auto* bu = static_cast<BlitUser*>(u);
        EdgePass* p = bu->edge;
        const int slot = p->writeOut % EdgePass::kOutRing;
        p->readyOut = slot;
        p->writeOut = (slot + 1) % EdgePass::kOutRing;
        const GpuImage& dst = p->out[slot];
        DeviceContext* ctx = p->ctx;
        transitionImage(ctx, cmd, src.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_READ_BIT,
                        VK_ACCESS_TRANSFER_READ_BIT);
        transitionImage(ctx, cmd, dst.image, VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[0] = {0, 0, 0};
        blit.srcOffsets[1] = {static_cast<int32_t>(src.width), static_cast<int32_t>(src.height), 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstOffsets[0] = {0, 0, 0};
        blit.dstOffsets[1] = {static_cast<int32_t>(dst.width), static_cast<int32_t>(dst.height), 1};
        ctx->vk.CmdBlitImage(cmd, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.image,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, bu->filter);
        transitionImage(ctx, cmd, dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT);
        return dst;
    };
    return submitCustom(fwd, back, fn, &user, appImage, waitSemCount, waitSems, waitStages,
                        appLayout);
}
