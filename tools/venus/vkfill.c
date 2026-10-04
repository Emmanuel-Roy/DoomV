// A Vulkan smoke test for -gpu=venus that needs no window: the GPU fills one
// buffer and copies it to another in host-visible memory, and the CPU checks
// every word of what the GPU wrote.
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("FAIL %s = %d\n", #x, r_); return 1; } } while (0)

static PFN_vkGetInstanceProcAddr gipa;
#define G(name) PFN_##name name = (PFN_##name)gipa(inst, #name)

int main(void)
{
	void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
	if (!lib) { printf("FAIL no libvulkan\n"); return 1; }
	gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
	VkInstance inst = VK_NULL_HANDLE;
	PFN_vkCreateInstance vkCreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
	VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vkfill", 1, NULL, 0, VK_API_VERSION_1_1};
	VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL};
	CHECK(vkCreateInstance(&ici, NULL, &inst));
	G(vkEnumeratePhysicalDevices); G(vkGetPhysicalDeviceProperties); G(vkGetPhysicalDeviceQueueFamilyProperties);
	G(vkGetPhysicalDeviceMemoryProperties); G(vkCreateDevice); G(vkGetDeviceProcAddr);
	uint32_t n = 8;
	VkPhysicalDevice pds[8];
	CHECK(vkEnumeratePhysicalDevices(inst, &n, pds));
	VkPhysicalDevice pd = VK_NULL_HANDLE;
	VkPhysicalDeviceProperties props;
	for (uint32_t i = 0; i < n; i++) {
		vkGetPhysicalDeviceProperties(pds[i], &props);
		if (strstr(props.deviceName, "Venus")) { pd = pds[i]; break; }
	}
	if (!pd) { printf("FAIL no Venus device\n"); return 1; }
	printf("device: %s\n", props.deviceName);
	uint32_t qn = 16;
	VkQueueFamilyProperties qf[16];
	vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qf);
	uint32_t family = 0;
	for (uint32_t i = 0; i < qn; i++) if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { family = i; break; }
	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, family, 1, &prio};
	VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci, 0, NULL, 0, NULL, NULL};
	VkDevice dev;
	CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
#define D(name) PFN_##name name = (PFN_##name)vkGetDeviceProcAddr(dev, #name)
	D(vkGetDeviceQueue); D(vkCreateBuffer); D(vkGetBufferMemoryRequirements); D(vkAllocateMemory);
	D(vkBindBufferMemory); D(vkMapMemory); D(vkCreateCommandPool); D(vkAllocateCommandBuffers);
	D(vkBeginCommandBuffer); D(vkCmdFillBuffer); D(vkCmdCopyBuffer); D(vkCmdPipelineBarrier);
	D(vkEndCommandBuffer); D(vkCreateFence); D(vkQueueSubmit); D(vkWaitForFences);
	VkQueue q;
	vkGetDeviceQueue(dev, family, 0, &q);

	const VkDeviceSize size = 4 << 20;   // 4 MB each
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(pd, &mp);
	VkBuffer buf[2];
	VkDeviceMemory mem[2];
	uint32_t *ptr[2];
	for (int b = 0; b < 2; b++) {
		VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size,
		                          VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		                          VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
		CHECK(vkCreateBuffer(dev, &bci, NULL, &buf[b]));
		VkMemoryRequirements mr;
		vkGetBufferMemoryRequirements(dev, buf[b], &mr);
		uint32_t type = ~0u;
		const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
		for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
			if ((mr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) { type = i; break; }
		if (type == ~0u) {
			printf("FAIL no host-visible memory; buffer types 0x%x, size %llu\n", mr.memoryTypeBits, (unsigned long long)mr.size);
			for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
				printf("  type %u: heap %u flags 0x%x\n", i, mp.memoryTypes[i].heapIndex, mp.memoryTypes[i].propertyFlags);
			return 1;
		}
		VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, type};
		CHECK(vkAllocateMemory(dev, &mai, NULL, &mem[b]));
		CHECK(vkBindBufferMemory(dev, buf[b], mem[b], 0));
		CHECK(vkMapMemory(dev, mem[b], 0, size, 0, (void **)&ptr[b]));
		memset(ptr[b], 0x11, size);
	}

	VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, family};
	VkCommandPool pool;
	CHECK(vkCreateCommandPool(dev, &pci, NULL, &pool));
	VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
	VkCommandBuffer cb;
	CHECK(vkAllocateCommandBuffers(dev, &cai, &cb));
	VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
	CHECK(vkBeginCommandBuffer(cb, &bi));
	vkCmdFillBuffer(cb, buf[0], 0, size, 0xC0FFEE42u);
	VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT};
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
	VkBufferCopy region = {0, 0, size};
	vkCmdCopyBuffer(cb, buf[0], buf[1], 1, &region);
	VkMemoryBarrier hb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, NULL, 0, NULL);
	CHECK(vkEndCommandBuffer(cb));
	VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, 0};
	VkFence fence;
	CHECK(vkCreateFence(dev, &fci, NULL, &fence));
	VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL};
	CHECK(vkQueueSubmit(q, 1, &si, fence));
	CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, ~0ull));

	size_t bad = 0;
	for (size_t i = 0; i < size / 4; i++) bad += ptr[0][i] != 0xC0FFEE42u || ptr[1][i] != 0xC0FFEE42u;
	printf("%s: GPU filled and copied %llu MB, %zu words wrong\n", bad ? "FAIL" : "PASS",
	       (unsigned long long)(size >> 20), bad);
	return bad != 0;
}
