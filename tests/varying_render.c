// Offscreen Vulkan check of the actual repaired modules on a real driver; no game content.
// Inputs are the synthetic shader pair from varying_repair.py (colour = clip position * 0.25).
#include <vulkan/vulkan.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../runtime/vk_varying.h"

#define OK(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) { fprintf(stderr, "%s: %d\n", #call, r_); return 1; } } while (0)
static VkDevice device;
static VkPhysicalDevice physical;
static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties p; vkGetPhysicalDeviceMemoryProperties(physical, &p);
    for (uint32_t i = 0; i < p.memoryTypeCount; i++)
        if ((bits >> i & 1) && (p.memoryTypes[i].propertyFlags & flags) == flags) return i;
    fprintf(stderr, "no suitable memory type\n"); exit(1);
}
static VkBuffer buffer(size_t bytes, VkBufferUsageFlags usage, VkDeviceMemory *memory, void **map) {
    VkBufferCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = bytes, .usage = usage};
    VkBuffer result; if (vkCreateBuffer(device, &ci, NULL, &result)) exit(1);
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(device, result, &req);
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
        .memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    if (vkAllocateMemory(device, &ai, NULL, memory) || vkBindBufferMemory(device, result, *memory, 0) ||
        vkMapMemory(device, *memory, 0, VK_WHOLE_SIZE, 0, map)) exit(1);
    return result;
}
static VkShaderModule module(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) exit(1);
    fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
    uint32_t *code = malloc((size_t)n); if (!code || fread(code, 1, (size_t)n, f) != (size_t)n) exit(1);
    fclose(f);
    VkShaderModuleCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = (size_t)n, .pCode = code};
    VkShaderModule result; if (vkCreateShaderModule(device, &ci, NULL, &result)) exit(1);
    free(code); return result;
}
int main(int argc, char **argv) {
    if (argc != 3) return 2;
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance instance; OK(vkCreateInstance(&ii, NULL, &instance));
    uint32_t count = 0; OK(vkEnumeratePhysicalDevices(instance, &count, NULL));
    VkPhysicalDevice devices[16]; if (!count || count > 16) return 2;
    OK(vkEnumeratePhysicalDevices(instance, &count, devices));
    VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR bary = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR};
    VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .pNext = &bary};
    VkPhysicalDeviceFeatures2 features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f13};
    for (uint32_t i = 0; i < count; i++) {
        vkGetPhysicalDeviceFeatures2(devices[i], &features);
        if (bary.fragmentShaderBarycentric && f13.dynamicRendering) { physical = devices[i]; break; }
    }
    if (!physical) { puts("SKIP: no barycentric-capable Vulkan device"); return 77; }
    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(physical, &props); printf("GPU: %s\n", props.deviceName);
    uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(physical, &nq, NULL);
    VkQueueFamilyProperties queues[64]; if (nq > 64) return 2;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &nq, queues);
    uint32_t family = 0; while (family < nq && !(queues[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) family++;
    if (family == nq) return 2;
    float priority = 1;
    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority};
    const char *extension = VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME;
    VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &features,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi, .enabledExtensionCount = 1, .ppEnabledExtensionNames = &extension};
    OK(vkCreateDevice(physical, &di, NULL, &device));
    VkQueue queue; vkGetDeviceQueue(device, family, 0, &queue);
    VkMemoryRequirements req;
    VkImageCreateInfo image_ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R32G32B32A32_SFLOAT, .extent = {64, 64, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    VkImage image; OK(vkCreateImage(device, &image_ci, NULL, &image)); vkGetImageMemoryRequirements(device, image, &req);
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
        .memoryTypeIndex = memory_type(req.memoryTypeBits, 0)};
    VkDeviceMemory image_mem; OK(vkAllocateMemory(device, &ai, NULL, &image_mem)); OK(vkBindImageMemory(device, image, image_mem, 0));
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = image, .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = image_ci.format, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView view; OK(vkCreateImageView(device, &vi, NULL, &view));
    VkDeviceMemory vbmem, ubmem, rbmem; void *vertex_data, *uniform_data, *readback;
    VkBuffer vb = buffer(48, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &vbmem, &vertex_data);
    VkBuffer ub = buffer(VK_VARYING_SUPPORT_BYTES, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &ubmem, &uniform_data);
    VkBuffer rb = buffer(64 * 64 * 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &rbmem, &readback);
    const float vertices[3][4] = {{-10000, -10000, 0, 1}, {10000, -10000, 0, 1}, {0, 10000, 0, -.5f}};
    memcpy(vertex_data, vertices, sizeof vertices);
    memset(uniform_data, 0, VK_VARYING_SUPPORT_BYTES);
    const float viewport_data[4] = {0, 64, 64, -64};
    memcpy((char *)uniform_data + VK_VARYING_VIEWPORT_OFFSET, viewport_data, sizeof viewport_data);
    VkDescriptorSetLayoutBinding binding = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &binding};
    VkDescriptorSetLayout set_layout; OK(vkCreateDescriptorSetLayout(device, &li, NULL, &set_layout));
    VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &set_layout};
    VkPipelineLayout layout; OK(vkCreatePipelineLayout(device, &pli, NULL, &layout));
    VkDescriptorPoolSize pool_size = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1};
    VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &pool_size};
    VkDescriptorPool pool; OK(vkCreateDescriptorPool(device, &dpi, NULL, &pool));
    VkDescriptorSetAllocateInfo dsi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = pool, .descriptorSetCount = 1, .pSetLayouts = &set_layout};
    VkDescriptorSet set; OK(vkAllocateDescriptorSets(device, &dsi, &set));
    VkDescriptorBufferInfo bi = {ub, 0, VK_VARYING_SUPPORT_BYTES};
    VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
        .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &bi};
    vkUpdateDescriptorSets(device, 1, &write, 0, NULL);
    VkShaderModule vm = module(argv[1]), fm = module(argv[2]);
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vm, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fm, .pName = "main"}};
    VkVertexInputBindingDescription vbind = {0, 16, VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attr = {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0};
    VkPipelineVertexInputStateCreateInfo vertex = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vbind, .vertexAttributeDescriptionCount = 1, .pVertexAttributeDescriptions = &attr};
    VkPipelineInputAssemblyStateCreateInfo assembly = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkViewport vp = {0, 64, 64, -64, 0, 1}; VkRect2D scissor = {{0, 0}, {64, 64}};
    VkPipelineViewportStateCreateInfo viewport = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &scissor};
    VkPipelineRasterizationStateCreateInfo raster = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo samples = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState ba = {.colorWriteMask = 15};
    VkPipelineColorBlendStateCreateInfo blend = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &ba};
    VkPipelineRenderingCreateInfo rendering = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1, .pColorAttachmentFormats = &image_ci.format};
    VkGraphicsPipelineCreateInfo pipeline_ci = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &rendering,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &vertex, .pInputAssemblyState = &assembly,
        .pViewportState = &viewport, .pRasterizationState = &raster, .pMultisampleState = &samples, .pColorBlendState = &blend, .layout = layout};
    VkPipeline pipeline; OK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_ci, NULL, &pipeline));
    VkCommandPoolCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = family};
    VkCommandPool cp; OK(vkCreateCommandPool(device, &cpi, NULL, &cp));
    VkCommandBufferAllocateInfo cba = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer cb; OK(vkAllocateCommandBuffers(device, &cba, &cb));
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; OK(vkBeginCommandBuffer(cb, &begin));
    VkImageMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = image, .subresourceRange = vi.subresourceRange};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
    VkRenderingAttachmentInfo attachment = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = view,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue.color.float32 = {-99, -99, -99, -99}};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = scissor, .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &attachment};
    vkCmdBeginRendering(cb, &ri); vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    VkDeviceSize zero = 0; vkCmdBindVertexBuffers(cb, 0, 1, &vb, &zero);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, NULL);
    vkCmdDraw(cb, 3, 1, 0, 0); vkCmdEndRendering(cb);
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {64, 64, 1}};
    vkCmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb, 1, &copy);
    VkMemoryBarrier host = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, NULL, 0, NULL);
    OK(vkEndCommandBuffer(cb));
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
    OK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE)); OK(vkQueueWaitIdle(queue));
    double worst = 0;
    for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) {
        double qx = (x + .5) / 32 - 1, qy = 1 - (y + .5) / 32;
        double w = 5000 / (20000 + 1.5 * qy), expected[4] = {qx * w * .25, qy * w * .25, 0, w * .25};
        const float *pixel = (const float *)readback + (y * 64 + x) * 4;
        for (int c = 0; c < 4; c++) { double error = fabs(pixel[c] - expected[c]); if (!isfinite(pixel[c])) error = 100; if (error > worst) worst = error; }
    }
    printf("4096 pixels checked; largest interpolation error %.9g\n", worst);
    // Process exit releases the isolated test's device and allocations.
    return worst < .002 ? 0 : 1;
}
