/*
 * tu_probe [library]: load Turnip as a Vulkan ICD (by default /redwood/lib/libvulkan_freedreno.dylib,
 * built by TURNIP=1 tools/mk-mesa.sh), list its physical devices and create a device with one queue.
 * On that device it fills a 1 MB buffer and copies it to another (three rounds), writes 4096 words
 * from a compute shader and draws a triangle over half of a 64x64 image, reading each result back
 * and counting the wrong values. Prints every result; exits 0 once the instance and enumeration worked.
 * A fault inside the library is printed with the faulting address and the library offset.
 */
#include <dlfcn.h>
#include <execinfo.h>
#include <signal.h>
#include <sys/ucontext.h>
#include <unistd.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

typedef VkResult (*negotiate_fn)(uint32_t *);
typedef PFN_vkVoidFunction (*gipa_fn)(VkInstance, const char *);

#include <time.h>
#include "shaders/fill_comp.h"
#include "shaders/tri_vert.h"
#include "shaders/tri_frag.h"

static double
now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

/* Fill one buffer on the GPU, copy it into a second, host-visible one, and check every word there. */
static void
fill_test(gipa_fn gipa, VkInstance instance, VkDevice device, const VkPhysicalDeviceMemoryProperties *m)
{
	PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)gipa(instance, "vkGetDeviceProcAddr");
#define DGET(name) PFN_##name name = (PFN_##name)gdpa(device, #name)
	DGET(vkGetDeviceQueue); DGET(vkCreateBuffer); DGET(vkGetBufferMemoryRequirements); DGET(vkAllocateMemory);
	DGET(vkBindBufferMemory); DGET(vkMapMemory); DGET(vkCreateCommandPool); DGET(vkAllocateCommandBuffers);
	DGET(vkBeginCommandBuffer); DGET(vkCmdFillBuffer); DGET(vkCmdCopyBuffer); DGET(vkCmdPipelineBarrier);
	DGET(vkEndCommandBuffer); DGET(vkCreateFence); DGET(vkQueueSubmit); DGET(vkWaitForFences);
	DGET(vkResetFences); DGET(vkResetCommandBuffer); DGET(vkDestroyFence); DGET(vkDestroyCommandPool);
	DGET(vkDestroyBuffer); DGET(vkFreeMemory); DGET(vkInvalidateMappedMemoryRanges);
	const VkDeviceSize size = 1 << 20;
	VkBuffer buf[2];
	VkDeviceMemory mem[2];
	VkResult r;

	VkQueue queue;
	vkGetDeviceQueue(device, 0, 0, &queue);
	for (int k = 0; k < 2; k++) {
		VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
		    .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT };
		r = vkCreateBuffer(device, &bci, NULL, &buf[k]);
		VkMemoryRequirements req;
		vkGetBufferMemoryRequirements(device, buf[k], &req);
		uint32_t type = UINT32_MAX;
		VkMemoryPropertyFlags want = k ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
		    : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
		for (uint32_t t = 0; t < m->memoryTypeCount; t++)
			if ((req.memoryTypeBits & (1u << t)) && (m->memoryTypes[t].propertyFlags & want) == want) { type = t; break; }
		VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
		    .memoryTypeIndex = type };
		VkResult a = vkAllocateMemory(device, &mai, NULL, &mem[k]);
		VkResult b = a == VK_SUCCESS ? vkBindBufferMemory(device, buf[k], mem[k], 0) : a;
		printf("tu_probe:   buffer %d: create %d, %llu bytes, memory type %u, allocate %d, bind %d\n", k, r,
		    (unsigned long long)req.size, type, a, b);
		if (r != VK_SUCCESS || b != VK_SUCCESS) return;
	}
	uint32_t *host;
	r = vkMapMemory(device, mem[1], 0, size, 0, (void **)&host);
	printf("tu_probe:   vkMapMemory %d\n", r);
	if (r != VK_SUCCESS) return;
	memset(host, 0, size);

	VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
	    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = 0 };
	VkCommandPool pool;
	vkCreateCommandPool(device, &pci, NULL, &pool);
	VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
	    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
	VkCommandBuffer cb;
	vkAllocateCommandBuffers(device, &cai, &cb);
	VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	VkFence fence;
	vkCreateFence(device, &fci, NULL, &fence);

	for (uint32_t round = 0; round < 3; round++) {
		uint32_t pattern = 0x5a5aa500u + round;
		VkCommandBufferBeginInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
		vkResetCommandBuffer(cb, 0);
		vkBeginCommandBuffer(cb, &cbi);
		vkCmdFillBuffer(cb, buf[0], 0, VK_WHOLE_SIZE, pattern);
		VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		    .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT };
		vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
		VkBufferCopy region = { 0, 0, size };
		vkCmdCopyBuffer(cb, buf[0], buf[1], 1, &region);
		VkMemoryBarrier hb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		    .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
		vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, NULL, 0, NULL);
		r = vkEndCommandBuffer(cb);
		VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
		double t0 = now_ms();
		VkResult s = vkQueueSubmit(queue, 1, &si, fence);
		VkResult w = s == VK_SUCCESS ? vkWaitForFences(device, 1, &fence, VK_TRUE, 2000000000ull) : s;
		double t1 = now_ms();
		uint32_t bad = 0, first_bad = 0;
		for (uint32_t j = 0; j < size / 4; j++)
			if (host[j] != pattern && bad++ == 0) first_bad = j;
		printf("tu_probe:   round %u: end %d, submit %d, wait %d in %.2f ms; %u of %u words differ from %#x (first at %u: %#x)\n",
		    round, r, s, w, t1 - t0, bad, (unsigned)(size / 4), pattern, first_bad, host[first_bad]);
		vkResetFences(device, 1, &fence);
		if (w != VK_SUCCESS) break;
	}
	vkDestroyFence(device, fence, NULL);
	vkDestroyCommandPool(device, pool, NULL);
	for (int k = 0; k < 2; k++) {
		vkDestroyBuffer(device, buf[k], NULL);
		vkFreeMemory(device, mem[k], NULL);
	}
}


/* Memory of a buffer or image: the first type with the wanted properties, bound; false on failure. */
static uint32_t
memory_type(const VkPhysicalDeviceMemoryProperties *m, uint32_t bits, VkMemoryPropertyFlags want)
{
	for (uint32_t t = 0; t < m->memoryTypeCount; t++)
		if ((bits & (1u << t)) && (m->memoryTypes[t].propertyFlags & want) == want) return t;
	return UINT32_MAX;
}

#define GPA(name) PFN_##name name = (PFN_##name)gdpa(device, #name)

/* A compute shader writes i * 3 + 1 to word i of a host-visible buffer; every word is checked. */
static void
compute_test(PFN_vkGetDeviceProcAddr gdpa, VkDevice device, const VkPhysicalDeviceMemoryProperties *m)
{
	GPA(vkGetDeviceQueue); GPA(vkCreateBuffer); GPA(vkGetBufferMemoryRequirements); GPA(vkAllocateMemory);
	GPA(vkBindBufferMemory); GPA(vkMapMemory); GPA(vkCreateShaderModule); GPA(vkCreateDescriptorSetLayout);
	GPA(vkCreatePipelineLayout); GPA(vkCreateComputePipelines); GPA(vkCreateDescriptorPool);
	GPA(vkAllocateDescriptorSets); GPA(vkUpdateDescriptorSets); GPA(vkCreateCommandPool);
	GPA(vkAllocateCommandBuffers); GPA(vkBeginCommandBuffer); GPA(vkCmdBindPipeline); GPA(vkCmdBindDescriptorSets);
	GPA(vkCmdDispatch); GPA(vkCmdPipelineBarrier); GPA(vkEndCommandBuffer); GPA(vkCreateFence); GPA(vkQueueSubmit);
	GPA(vkWaitForFences);
	const uint32_t words = 64 * 64;
	VkQueue queue;
	vkGetDeviceQueue(device, 0, 0, &queue);
	VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = words * 4,
	    .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT };
	VkBuffer buf;
	VkDeviceMemory mem;
	VkMemoryRequirements req;
	uint32_t *host;
	vkCreateBuffer(device, &bci, NULL, &buf);
	vkGetBufferMemoryRequirements(device, buf, &req);
	VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
	    .memoryTypeIndex = memory_type(m, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
	if (vkAllocateMemory(device, &mai, NULL, &mem) != VK_SUCCESS || vkBindBufferMemory(device, buf, mem, 0) != VK_SUCCESS ||
	    vkMapMemory(device, mem, 0, VK_WHOLE_SIZE, 0, (void **)&host) != VK_SUCCESS) {
		printf("tu_probe:   compute: no host-visible storage buffer\n");
		return;
	}
	memset(host, 0, words * 4);
	VkShaderModuleCreateInfo smi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof(spv_fill_comp),
	    .pCode = spv_fill_comp };
	VkShaderModule module;
	VkResult r = vkCreateShaderModule(device, &smi, NULL, &module);
	VkDescriptorSetLayoutBinding binding = { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL };
	VkDescriptorSetLayoutCreateInfo dli = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1,
	    .pBindings = &binding };
	VkDescriptorSetLayout dsl;
	vkCreateDescriptorSetLayout(device, &dli, NULL, &dsl);
	VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
	    .pSetLayouts = &dsl };
	VkPipelineLayout layout;
	vkCreatePipelineLayout(device, &pli, NULL, &layout);
	VkComputePipelineCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
	    .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
	    .module = module, .pName = "main" }, .layout = layout };
	VkPipeline pipeline;
	double t0 = now_ms();
	VkResult pr = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpi, NULL, &pipeline);
	printf("tu_probe:   compute: shader module %d, pipeline %d in %.2f ms\n", r, pr, now_ms() - t0);
	if (pr != VK_SUCCESS) return;
	VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 };
	VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1,
	    .pPoolSizes = &ps };
	VkDescriptorPool dpool;
	vkCreateDescriptorPool(device, &dpi, NULL, &dpool);
	VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dpool,
	    .descriptorSetCount = 1, .pSetLayouts = &dsl };
	VkDescriptorSet set;
	vkAllocateDescriptorSets(device, &dai, &set);
	VkDescriptorBufferInfo dbi = { buf, 0, VK_WHOLE_SIZE };
	VkWriteDescriptorSet w = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .descriptorCount = 1,
	    .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi };
	vkUpdateDescriptorSets(device, 1, &w, 0, NULL);
	VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = 0 };
	VkCommandPool pool;
	vkCreateCommandPool(device, &pci, NULL, &pool);
	VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
	    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
	VkCommandBuffer cb;
	vkAllocateCommandBuffers(device, &cai, &cb);
	VkCommandBufferBeginInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	vkBeginCommandBuffer(cb, &cbi);
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
	vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL);
	vkCmdDispatch(cb, words / 64, 1, 1);
	VkMemoryBarrier hb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
	    .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, NULL, 0, NULL);
	vkEndCommandBuffer(cb);
	VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	VkFence fence;
	vkCreateFence(device, &fci, NULL, &fence);
	VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
	t0 = now_ms();
	VkResult s = vkQueueSubmit(queue, 1, &si, fence);
	VkResult wt = s == VK_SUCCESS ? vkWaitForFences(device, 1, &fence, VK_TRUE, 2000000000ull) : s;
	uint32_t bad = 0, first = 0;
	for (uint32_t i = 0; i < words; i++)
		if (host[i] != i * 3 + 1 && bad++ == 0) first = i;
	printf("tu_probe:   compute: submit %d, wait %d in %.2f ms; %u of %u words wrong (first at %u: %#x)\n", s, wt,
	    now_ms() - t0, bad, words, first, host[first]);
}

/* A triangle over the top-left half of a 64 x 64 image cleared black, copied out and counted. */
static void
draw_test(PFN_vkGetDeviceProcAddr gdpa, VkDevice device, const VkPhysicalDeviceMemoryProperties *m)
{
	GPA(vkGetDeviceQueue); GPA(vkCreateBuffer); GPA(vkGetBufferMemoryRequirements); GPA(vkAllocateMemory);
	GPA(vkBindBufferMemory); GPA(vkMapMemory); GPA(vkCreateImage); GPA(vkGetImageMemoryRequirements);
	GPA(vkBindImageMemory); GPA(vkCreateImageView); GPA(vkCreateRenderPass); GPA(vkCreateFramebuffer);
	GPA(vkCreateShaderModule); GPA(vkCreatePipelineLayout); GPA(vkCreateGraphicsPipelines); GPA(vkCreateCommandPool);
	GPA(vkAllocateCommandBuffers); GPA(vkBeginCommandBuffer); GPA(vkCmdBeginRenderPass); GPA(vkCmdBindPipeline);
	GPA(vkCmdDraw); GPA(vkCmdEndRenderPass); GPA(vkCmdCopyImageToBuffer); GPA(vkCmdPipelineBarrier);
	GPA(vkEndCommandBuffer); GPA(vkCreateFence); GPA(vkQueueSubmit); GPA(vkWaitForFences);
	const uint32_t side = 64;
	VkQueue queue;
	vkGetDeviceQueue(device, 0, 0, &queue);
	VkImageCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
	    .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = { side, side, 1 }, .mipLevels = 1, .arrayLayers = 1,
	    .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
	    .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT };
	VkImage image;
	VkDeviceMemory imem, bmem;
	VkMemoryRequirements req;
	vkCreateImage(device, &ici, NULL, &image);
	vkGetImageMemoryRequirements(device, image, &req);
	VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
	    .memoryTypeIndex = memory_type(m, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
	if (vkAllocateMemory(device, &mai, NULL, &imem) != VK_SUCCESS || vkBindImageMemory(device, image, imem, 0) != VK_SUCCESS) {
		printf("tu_probe:   draw: no image memory\n");
		return;
	}
	VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = side * side * 4,
	    .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT };
	VkBuffer buf;
	uint8_t *host;
	vkCreateBuffer(device, &bci, NULL, &buf);
	vkGetBufferMemoryRequirements(device, buf, &req);
	mai.allocationSize = req.size;
	mai.memoryTypeIndex = memory_type(m, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	if (vkAllocateMemory(device, &mai, NULL, &bmem) != VK_SUCCESS || vkBindBufferMemory(device, buf, bmem, 0) != VK_SUCCESS ||
	    vkMapMemory(device, bmem, 0, VK_WHOLE_SIZE, 0, (void **)&host) != VK_SUCCESS) {
		printf("tu_probe:   draw: no host-visible buffer\n");
		return;
	}
	memset(host, 0x55, side * side * 4);
	VkImageViewCreateInfo vci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = image,
	    .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
	    .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
	VkImageView view;
	vkCreateImageView(device, &vci, NULL, &view);
	VkAttachmentDescription ad = { .format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
	    .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
	    .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
	    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL };
	VkAttachmentReference ar = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkSubpassDescription sd = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1,
	    .pColorAttachments = &ar };
	VkSubpassDependency dep = { 0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
	    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, 0 };
	VkRenderPassCreateInfo rpi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1,
	    .pAttachments = &ad, .subpassCount = 1, .pSubpasses = &sd, .dependencyCount = 1, .pDependencies = &dep };
	VkRenderPass pass;
	vkCreateRenderPass(device, &rpi, NULL, &pass);
	VkFramebufferCreateInfo fbi = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = pass,
	    .attachmentCount = 1, .pAttachments = &view, .width = side, .height = side, .layers = 1 };
	VkFramebuffer fb;
	vkCreateFramebuffer(device, &fbi, NULL, &fb);
	VkShaderModuleCreateInfo vsi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof(spv_tri_vert),
	    .pCode = spv_tri_vert };
	VkShaderModuleCreateInfo fsi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof(spv_tri_frag),
	    .pCode = spv_tri_frag };
	VkShaderModule vs, fs;
	vkCreateShaderModule(device, &vsi, NULL, &vs);
	vkCreateShaderModule(device, &fsi, NULL, &fs);
	VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	VkPipelineLayout layout;
	vkCreatePipelineLayout(device, &pli, NULL, &layout);
	VkPipelineShaderStageCreateInfo stages[2] = {
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main" },
		{ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main" },
	};
	VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
	    .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
	VkViewport vp = { 0, 0, side, side, 0, 1 };
	VkRect2D sc = { { 0, 0 }, { side, side } };
	VkPipelineViewportStateCreateInfo vps = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
	    .viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc };
	VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
	    .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
	    .lineWidth = 1 };
	VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
	    .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
	VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xf };
	VkPipelineColorBlendStateCreateInfo cb_state = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
	    .attachmentCount = 1, .pAttachments = &cba };
	VkGraphicsPipelineCreateInfo gpi = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2,
	    .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vps,
	    .pRasterizationState = &rs, .pMultisampleState = &ms, .pColorBlendState = &cb_state, .layout = layout,
	    .renderPass = pass };
	VkPipeline pipeline;
	double t0 = now_ms();
	VkResult pr = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpi, NULL, &pipeline);
	printf("tu_probe:   draw: graphics pipeline %d in %.2f ms\n", pr, now_ms() - t0);
	if (pr != VK_SUCCESS) return;
	VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = 0 };
	VkCommandPool pool;
	vkCreateCommandPool(device, &pci, NULL, &pool);
	VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
	    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
	VkCommandBuffer cb;
	vkAllocateCommandBuffers(device, &cai, &cb);
	VkCommandBufferBeginInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	vkBeginCommandBuffer(cb, &cbi);
	VkClearValue clear = { .color = { .float32 = { 0, 0, 0, 1 } } };
	VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = pass, .framebuffer = fb,
	    .renderArea = sc, .clearValueCount = 1, .pClearValues = &clear };
	vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
	vkCmdDraw(cb, 3, 1, 0, 0);
	vkCmdEndRenderPass(cb);
	VkBufferImageCopy region = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { side, side, 1 } };
	vkCmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &region);
	VkMemoryBarrier hb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
	    .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, NULL, 0, NULL);
	vkEndCommandBuffer(cb);
	VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	VkFence fence;
	vkCreateFence(device, &fci, NULL, &fence);
	VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
	t0 = now_ms();
	VkResult s = vkQueueSubmit(queue, 1, &si, fence);
	VkResult wt = s == VK_SUCCESS ? vkWaitForFences(device, 1, &fence, VK_TRUE, 2000000000ull) : s;
	double took = now_ms() - t0;
	uint32_t coloured = 0, black = 0, other = 0, wrong_side = 0;
	for (uint32_t y = 0; y < side; y++)
		for (uint32_t x = 0; x < side; x++) {
			const uint8_t *px = host + (y * side + x) * 4;
			bool c = abs(px[0] - 64) <= 1 && abs(px[1] - 128) <= 1 && abs(px[2] - 191) <= 1 && px[3] == 255;
			bool k = px[0] == 0 && px[1] == 0 && px[2] == 0 && px[3] == 255;
			coloured += c; black += k; other += !c && !k;
			/* well inside either half: x + y below side - 2 is the triangle, above side is not */
			if ((x + y + 2 < side && !c) || (x + y > side && !k)) wrong_side++;
		}
	printf("tu_probe:   draw: submit %d, wait %d in %.2f ms; %u coloured, %u black, %u other, %u on the wrong side; "
	    "pixel (2,2) %02x%02x%02x%02x, (61,61) %02x%02x%02x%02x\n", s, wt, took, coloured, black, other, wrong_side,
	    host[(2 * side + 2) * 4], host[(2 * side + 2) * 4 + 1], host[(2 * side + 2) * 4 + 2], host[(2 * side + 2) * 4 + 3],
	    host[(61 * side + 61) * 4], host[(61 * side + 61) * 4 + 1], host[(61 * side + 61) * 4 + 2], host[(61 * side + 61) * 4 + 3]);
}


/* A trap in the driver: where it was, as offsets into the image it is in, for symbolizing on the host. */
static void
trapped(int sig, siginfo_t *info, void *context)
{
	ucontext_t *uc = context;
	uint64_t pc = uc->uc_mcontext->__ss.__pc, lr = uc->uc_mcontext->__ss.__lr;
	void *frames[32];
	int n = backtrace(frames, 32);
	Dl_info d;

	(void)info;
	if (dladdr((void *)pc, &d))
		dprintf(2, "tu_probe: signal %d at %s+%#llx (%s+%#llx)\n", sig, d.dli_fname,
		    (unsigned long long)(pc - (uint64_t)d.dli_fbase), d.dli_sname ? d.dli_sname : "?",
		    (unsigned long long)(pc - (uint64_t)d.dli_saddr));
	if (dladdr((void *)lr, &d))
		dprintf(2, "tu_probe: lr %s+%#llx\n", d.dli_fname, (unsigned long long)(lr - (uint64_t)d.dli_fbase));
	for (int i = 0; i < n; i++)
		if (dladdr(frames[i], &d))
			dprintf(2, "tu_probe: frame %d %s+%#llx\n", i, d.dli_fname,
			    (unsigned long long)((uint64_t)frames[i] - (uint64_t)d.dli_fbase));
	_exit(128 + sig);
}

int
main(int argc, char **argv)
{
	struct sigaction sa = { .sa_sigaction = trapped, .sa_flags = SA_SIGINFO };
	sigaction(SIGILL, &sa, NULL);
	sigaction(SIGSEGV, &sa, NULL);
	sigaction(SIGBUS, &sa, NULL);
	sigaction(SIGTRAP, &sa, NULL);
	sigaction(SIGABRT, &sa, NULL);
	setvbuf(stdout, NULL, _IONBF, 0);
	const char *path = argc > 1 ? argv[1] : "/redwood/lib/libvulkan_freedreno.dylib";
	setenv("TU_DEBUG", "startup", 0);
	void *lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (lib == NULL) {
		printf("tu_probe: dlopen %s: %s\n", path, dlerror());
		return 1;
	}
	negotiate_fn negotiate = (negotiate_fn)dlsym(lib, "vk_icdNegotiateLoaderICDInterfaceVersion");
	gipa_fn gipa = (gipa_fn)dlsym(lib, "vk_icdGetInstanceProcAddr");
	uint32_t icd = 5;
	if (negotiate == NULL || gipa == NULL || negotiate(&icd) != VK_SUCCESS) {
		printf("tu_probe: not an ICD (negotiate %p, gipa %p)\n", (void *)negotiate, (void *)gipa);
		return 1;
	}
	printf("tu_probe: ICD interface %u\n", icd);

	PFN_vkCreateInstance create_instance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
	VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "tu_probe",
	    .apiVersion = VK_API_VERSION_1_3 };
	VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
	VkInstance instance;
	VkResult r = create_instance(&ici, NULL, &instance);
	printf("tu_probe: vkCreateInstance %d\n", r);
	if (r != VK_SUCCESS) return 1;

#define GET(name) PFN_##name name = (PFN_##name)gipa(instance, #name)
	GET(vkEnumeratePhysicalDevices);
	GET(vkGetPhysicalDeviceProperties);
	GET(vkGetPhysicalDeviceMemoryProperties);
	GET(vkGetPhysicalDeviceQueueFamilyProperties);
	GET(vkGetPhysicalDeviceFeatures);
	GET(vkCreateDevice);
	GET(vkDestroyDevice);
	GET(vkDestroyInstance);

	uint32_t count = 0;
	r = vkEnumeratePhysicalDevices(instance, &count, NULL);
	printf("tu_probe: vkEnumeratePhysicalDevices %d, %u devices\n", r, count);
	if (r != VK_SUCCESS || count == 0) return 1;
	VkPhysicalDevice pd[4];
	if (count > 4) count = 4;
	vkEnumeratePhysicalDevices(instance, &count, pd);

	for (uint32_t i = 0; i < count; i++) {
		VkPhysicalDeviceProperties p;
		vkGetPhysicalDeviceProperties(pd[i], &p);
		printf("tu_probe: device %u: %s, API %u.%u.%u, driver %#x, vendor %#x, device %#x, type %d\n", i,
		    p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion),
		    VK_API_VERSION_PATCH(p.apiVersion), p.driverVersion, p.vendorID, p.deviceID, p.deviceType);
		printf("tu_probe:   maxImageDimension2D %u, maxComputeWorkGroupInvocations %u, timestampPeriod %.2f ns\n",
		    p.limits.maxImageDimension2D, p.limits.maxComputeWorkGroupInvocations, p.limits.timestampPeriod);
		VkPhysicalDeviceFeatures f;
		vkGetPhysicalDeviceFeatures(pd[i], &f);
		printf("tu_probe:   geometryShader %u, tessellationShader %u, sparseBinding %u, textureCompressionASTC_LDR %u\n",
		    f.geometryShader, f.tessellationShader, f.sparseBinding, f.textureCompressionASTC_LDR);
		VkPhysicalDeviceMemoryProperties m;
		vkGetPhysicalDeviceMemoryProperties(pd[i], &m);
		for (uint32_t h = 0; h < m.memoryHeapCount; h++)
			printf("tu_probe:   heap %u: %llu MB, flags %#x\n", h, (unsigned long long)(m.memoryHeaps[h].size >> 20),
			    m.memoryHeaps[h].flags);
		uint32_t qn = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(pd[i], &qn, NULL);
		VkQueueFamilyProperties q[8];
		if (qn > 8) qn = 8;
		vkGetPhysicalDeviceQueueFamilyProperties(pd[i], &qn, q);
		for (uint32_t k = 0; k < qn; k++)
			printf("tu_probe:   queue family %u: flags %#x, %u queues\n", k, q[k].queueFlags, q[k].queueCount);

		float priority = 1.0f;
		VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0,
		    .queueCount = 1, .pQueuePriorities = &priority };
		VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
		    .pQueueCreateInfos = &qci };
		VkDevice device;
		r = vkCreateDevice(pd[i], &dci, NULL, &device);
		printf("tu_probe:   vkCreateDevice %d\n", r);
		if (r == VK_SUCCESS) {
			fill_test(gipa, instance, device, &m);
			PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)gipa(instance, "vkGetDeviceProcAddr");
			compute_test(gdpa, device, &m);
			draw_test(gdpa, device, &m);
			vkDestroyDevice(device, NULL);
		}
	}
	vkDestroyInstance(instance, NULL);
	printf("tu_probe: done\n");
	return 0;
}
