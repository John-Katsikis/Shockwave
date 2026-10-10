#include "RendererImpl.h"

namespace shockwave {

void VulkanRenderer::Impl::initGui() {
    guiColorFormat = swapchain->format();

    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = VK_API_VERSION_1_3;
    info.Instance = context->instance();
    info.PhysicalDevice = context->physicalDevice();
    info.Device = context->device();
    info.QueueFamily = context->graphicsQueueFamily();
    info.Queue = context->graphicsQueue();
    info.DescriptorPoolSize = 1024;   // the backend owns its pool: one set per GUI texture
    info.MinImageCount = 2;
    // ImGui rotates its vertex buffers per frame; it needs at least one per frame in flight.
    info.ImageCount = std::max(kFramesInFlight, swapchain->imageCount());
    info.UseDynamicRendering = true;
    info.PipelineInfoMain.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    info.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    info.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &guiColorFormat;
    // Windows of panels dragged out of the main one: the backend makes their swapchains,
    // preferring the main window's format so colors match.
    info.PipelineInfoForViewports.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    info.PipelineInfoForViewports.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    info.PipelineInfoForViewports.PipelineRenderingCreateInfo.pColorAttachmentFormats = &guiColorFormat;
    info.CheckVkResultFn = [](VkResult result) { vk::check(result, "Dear ImGui Vulkan backend"); };
    if (!ImGui_ImplVulkan_Init(&info)) throw std::runtime_error("Initializing the Dear ImGui Vulkan backend failed");

    guiEnabled = true;
    registerGuiSceneTexture();
}

void VulkanRenderer::Impl::shutdownGui() {
    if (!guiEnabled) return;
    vkDeviceWaitIdle(context->device());
    if (guiSceneTexture) ImGui_ImplVulkan_RemoveTexture(guiSceneTexture);
    guiSceneTexture = VK_NULL_HANDLE;
    for (auto& [id, set] : guiTextures) ImGui_ImplVulkan_RemoveTexture(set);
    guiTextures.clear();
    ImGui_ImplVulkan_Shutdown();
    guiEnabled = false;
    if (viewportSize.width) needsSceneResize = true;   // back to window-sized output
}

void VulkanRenderer::Impl::renderGuiWindows() {
    if (!guiEnabled || !(ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable)) return;
    // The backend creates swapchains (waiting for the device idle), submits and presents on
    // the graphics queue for each window.
    std::lock_guard lock(queueMutex);
    ImGui::UpdatePlatformWindows();
    ImGui::RenderPlatformWindowsDefault();
}

void VulkanRenderer::Impl::recordGui(VkCommandBuffer cmd, VkImageView target, bool clear) {
    VkRenderingAttachmentInfo color{};
    color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color.imageView = target;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};

    VkRenderingInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea = {{0, 0}, swapchain->extent()};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;
    vkCmdBeginRendering(cmd, &rendering);
    if (ImDrawData* data = ImGui::GetDrawData()) {
        // The backend may upload font and UI textures on the queue from inside this call.
        std::lock_guard lock(queueMutex);
        ImGui_ImplVulkan_RenderDrawData(data, cmd);
    }
    vkCmdEndRendering(cmd);
}

}   // namespace shockwave
