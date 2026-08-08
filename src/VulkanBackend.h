/*
 * scvk - a native Vulkan renderer for SimCity 4
 *
 * Copyright (C) 2026 aspctt
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation, under
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <https://www.gnu.org/licenses/>.
 */

#pragma once
#include "VulkanApi.h"

#include <string>
#include <vector>

namespace scvk
{
	/**
	 * The Vulkan device and swapchain behind the driver.
	 *
	 * This first version does only what the game's startup and loading screens
	 * need: clear the image, copy blocks of pixels into it, and present. That
	 * is deliberately a transfer-only path. The game hands over BGRA8 pixels
	 * and the Windows swapchain is natively B8G8R8A8_UNORM, so the data can go
	 * straight in with no conversion, which means no shaders, no render pass,
	 * no pipeline and no descriptor sets.
	 *
	 * The 3D path will need all of those, but none of it is required to get a
	 * picture on screen, and building it later against a working swapchain is
	 * far easier than building it at the same time.
	 *
	 * Every entry point is safe to call when initialisation failed. The game
	 * cannot be allowed to crash because a machine has no Vulkan driver, so a
	 * dead backend simply does nothing and says so once in the log.
	 */
	class VulkanBackend
	{
	public:
		VulkanBackend(void);
		~VulkanBackend(void);

		VulkanBackend(VulkanBackend const&) = delete;
		VulkanBackend& operator=(VulkanBackend const&) = delete;

		/** Loads Vulkan, creates the instance, and picks a physical device. */
		bool CreateInstance(void);

		/** Creates the surface, device and swapchain for a window. */
		bool CreateSurfaceAndDevice(void* hwnd, uint32_t width, uint32_t height);

		void DestroySwapchain(void);
		void Destroy(void);

		bool IsReady(void) const { return swapchain != VK_NULL_HANDLE; }

		/** Description of the selected GPU, for GetDriverInfo. */
		std::string const& DeviceName(void) const { return deviceName; }
		std::string const& ApiVersion(void) const { return apiVersion; }

		void Clear(float r, float g, float b, float a);

		/**
		 * Copies tightly packed BGRA8 pixels into the current image.
		 *
		 * srcWidth is the row stride of the source in pixels, which matters
		 * when the destination is clipped: the rows still have to be strided
		 * by the full source width.
		 */
		void BlitPixels(int32_t destX, int32_t destY, uint32_t width, uint32_t height,
			uint32_t srcWidth, void const* pixels);

		/** Ends the frame, submits, and presents. */
		void Present(void);

		/**
		 * Sets the transform for subsequent draws.
		 *
		 * Takes the game's combined projection times modelview in OpenGL
		 * column-major convention. The correction into Vulkan clip space is
		 * applied here rather than by the caller, because it is a property of
		 * the API rather than of the game.
		 */
		void SetTransform(float const* glMvp);

		/**
		 * Sets the viewport, in the game's OpenGL convention.
		 *
		 * The game renders parts of the interface into sub-rectangles and
		 * pairs each with a projection matched to that rectangle, so ignoring
		 * this stretches a small region across the whole window.
		 *
		 * y is measured from the bottom, as OpenGL does it; Vulkan measures
		 * from the top, and the flip happens here.
		 */
		void SetViewport(int32_t x, int32_t y, int32_t width, int32_t height);

		/** Resets the viewport to the whole window. */
		void SetFullViewport(void);

		/**
		 * Writes the next presented frame to a file, as a 32 bit BMP.
		 *
		 * Captured from the swapchain image rather than from the screen. That
		 * is the only way to be sure the picture is what scvk drew: grabbing
		 * the desktop needs the window focused, cannot see a Vulkan surface
		 * reliably, and captures whatever else happens to be on screen.
		 *
		 * BMP because its 32 bit layout is already BGRA, matching the
		 * swapchain exactly, so no encoder and no conversion are needed.
		 */
		void RequestCapture(char const* path);

		/**
		 * Draws from client memory.
		 *
		 * The interface hands over a plain pointer and a stride, so the data
		 * is copied into a per-frame buffer before each draw. Quads have no
		 * Vulkan equivalent and are drawn indexed as triangle pairs.
		 */
		void DrawVertices(uint32_t gdPrimType, uint32_t gdVertexFormat,
			void const* vertices, uint32_t firstVertex, uint32_t vertexCount);

		/**
		 * Creates a texture and returns a handle, or 0 on failure.
		 *
		 * gdInternalFormat is the game's own enumeration: 0 to 4 are
		 * uncompressed and 5 to 7 are DXT1, DXT3 and DXT5.
		 */
		uint32_t CreateTexture(uint32_t gdInternalFormat, uint32_t width, uint32_t height, uint32_t levels);

		/** Uploads one level, or a rectangle of one level. */
		void UploadTextureLevel(uint32_t handle, uint32_t level,
			int32_t xoffset, int32_t yoffset, uint32_t width, uint32_t height,
			uint32_t gdFormat, uint32_t gdType, uint32_t rowLength, void const* pixels);

		/** Selects the texture used by subsequent draws. 0 means untextured. */
		void SetTexture(uint32_t handle);

		void DestroyTexture(uint32_t handle);

	private:
		/**
		 * Everything that distinguishes one pipeline from another.
		 *
		 * Tiny for now: the game only ever supplies two vertex strides, and
		 * only two primitive topologies survive translation. This is the seed
		 * of the real state key that the fixed function emulation will need,
		 * which will grow to cover blending, depth, alpha test and the texture
		 * combiners.
		 */
		/**
		 * What distinguishes one pipeline from another.
		 *
		 * Keyed on the game's vertex format id rather than the stride. Stride
		 * is not enough: V3F_C4UB_T2F and V3F_N3F are both 24 bytes but agree
		 * on nothing after the position, so keying on size alone silently
		 * reads normals as colours.
		 */
		struct PipelineKey
		{
			uint32_t            format;
			VkPrimitiveTopology topology;

			bool operator==(PipelineKey const& other) const
			{
				return format == other.format && topology == other.topology;
			}
		};

		/** Where a format's attributes live, decoded once per format. */
		struct VertexLayout
		{
			uint32_t stride         = 0;
			bool     hasColour      = false;
			uint32_t colourOffset   = 0;
			bool     hasTexCoord    = false;
			uint32_t texCoordOffset = 0;
		};

		/** A texture, its view, and the descriptor set that binds it. */
		struct Texture
		{
			VkImage         image      = VK_NULL_HANDLE;
			VkDeviceMemory  memory     = VK_NULL_HANDLE;
			VkImageView     view       = VK_NULL_HANDLE;
			VkDescriptorSet descriptor = VK_NULL_HANDLE;
			VkFormat        format     = VK_FORMAT_UNDEFINED;
			uint32_t        width      = 0;
			uint32_t        height     = 0;
			uint32_t        levels     = 1;
			bool            compressed = false;
			bool            live       = false;
		};

		struct PipelineEntry
		{
			PipelineKey key;
			VkPipeline  pipeline;
		};

	private:
		bool PickPhysicalDevice(void);
		bool CreateLogicalDevice(void);
		bool CreateSwapchain(uint32_t width, uint32_t height);
		bool CreateFrameResources(void);
		bool CreateStagingBuffer(VkDeviceSize size);

		bool CreateRenderPass(void);
		bool CreateFramebuffers(void);
		void DestroyFramebuffers(void);
		bool CreateShaderModules(void);
		bool CreatePipelineLayout(void);
		bool CreateGeometryBuffers(void);
		void DestroyPipelines(void);

		bool CreateDescriptorResources(void);
		bool CreateDefaultTexture(void);
		void DestroyTextures(void);

		/** Runs a one-off command buffer to completion. Used for uploads. */
		bool SubmitImmediate(void (*record)(VkCommandBuffer, void*), void* context);

		/** Where a format's attributes live, from the game's packed encoding. */
		static VertexLayout DecodeVertexLayout(uint32_t gdVertexFormat);

		VkPipeline GetPipeline(PipelineKey const& key);

		/** Starts a frame if one is not already in progress. */
		bool EnsureFrame(void);

		void BeginRenderPassIfNeeded(void);
		void ApplyViewport(void);
		void EndRenderPassIfActive(void);

		/** Barriers the swapchain image into a layout, tracking where it was. */
		void TransitionTo(VkImageLayout newLayout);

		bool FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags properties, uint32_t& outIndex) const;

		bool CreateHostBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
			VkBuffer& outBuffer, VkDeviceMemory& outMemory, void*& outMapped);

		/** Logs once per distinct failure, then goes quiet. */
		void Fail(char const* what, VkResult result);

	private:
		VkDebugUtilsMessengerEXT debugMessenger = VK_NULL_HANDLE;

		VkInstance       instance       = VK_NULL_HANDLE;
		VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
		VkDevice         device         = VK_NULL_HANDLE;
		VkQueue          queue          = VK_NULL_HANDLE;
		uint32_t         queueFamily    = UINT32_MAX;

		VkSurfaceKHR   surface   = VK_NULL_HANDLE;
		VkSwapchainKHR swapchain = VK_NULL_HANDLE;
		VkFormat       swapchainFormat = VK_FORMAT_UNDEFINED;
		VkExtent2D     swapchainExtent{};
		std::vector<VkImage> swapchainImages;

		VkCommandPool   commandPool   = VK_NULL_HANDLE;
		VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
		VkSemaphore     imageAvailable = VK_NULL_HANDLE;
		VkSemaphore     renderFinished = VK_NULL_HANDLE;
		VkFence         inFlight       = VK_NULL_HANDLE;

		// Staging memory for pixel uploads. Commands are recorded now and
		// executed later, so the bytes have to stay put until the submit
		// completes. Each blit takes the next slice and the whole thing is
		// rewound when a frame begins.
		VkBuffer       stagingBuffer = VK_NULL_HANDLE;
		VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
		VkDeviceSize   stagingSize   = 0;
		VkDeviceSize   stagingUsed   = 0;
		void*          stagingMapped = nullptr;

		// Per-frame vertex data, bump allocated and rewound each frame for the
		// same reason as the staging buffer: the draw is recorded now and runs
		// later, so the bytes have to stay put until the submit completes.
		VkBuffer       vertexBuffer = VK_NULL_HANDLE;
		VkDeviceMemory vertexMemory = VK_NULL_HANDLE;
		VkDeviceSize   vertexSize   = 0;
		VkDeviceSize   vertexUsed   = 0;
		void*          vertexMapped = nullptr;

		// Static indices turning consecutive quads into triangle pairs.
		VkBuffer       quadIndexBuffer = VK_NULL_HANDLE;
		VkDeviceMemory quadIndexMemory = VK_NULL_HANDLE;
		uint32_t       quadCapacity    = 0;

		VkRenderPass               renderPass = VK_NULL_HANDLE;
		std::vector<VkImageView>   swapchainImageViews;
		std::vector<VkFramebuffer> framebuffers;

		// Indexed by (hasColour << 1) | hasTexCoord.
		VkShaderModule   vertModules[4] = {};
		VkShaderModule   fragModule     = VK_NULL_HANDLE;
		VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
		std::vector<PipelineEntry> pipelines;

		VkDescriptorSetLayout descriptorLayout = VK_NULL_HANDLE;
		VkDescriptorPool      descriptorPool   = VK_NULL_HANDLE;
		VkSampler             sampler          = VK_NULL_HANDLE;

		// Index 0 is a 1x1 white texture, so an untextured draw multiplies by
		// one instead of needing its own shader and pipeline.
		std::vector<Texture> textures;
		uint32_t             currentTexture = 0;

		VkCommandBuffer uploadCommandBuffer = VK_NULL_HANDLE;
		VkFence         uploadFence         = VK_NULL_HANDLE;

		VkImageLayout currentLayout    = VK_IMAGE_LAYOUT_UNDEFINED;
		bool          renderPassActive = false;

		// In the game's coordinates, converted when a draw applies them.
		// Negative width means "not set yet", so the full window is used.
		int32_t viewportX      = 0;
		int32_t viewportY      = 0;
		int32_t viewportWidth  = -1;
		int32_t viewportHeight = -1;

		float transform[16] = {
			1, 0, 0, 0,
			0, 1, 0, 0,
			0, 0, 1, 0,
			0, 0, 0, 1,
		};

		void*    windowHandle = nullptr;
		uint32_t imageIndex   = 0;
		bool     frameActive  = false;
		bool     dead         = false;

		// Reused when a Flush arrives with no frame started, so the swapchain
		// keeps cycling instead of stalling.
		float lastClearColour[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

		uint64_t presentedFrames = 0;

		// Readback for RequestCapture. Allocated on first use and kept, since
		// captures come in small numbers and the buffer is large.
		VkBuffer       readbackBuffer = VK_NULL_HANDLE;
		VkDeviceMemory readbackMemory = VK_NULL_HANDLE;
		void*          readbackMapped = nullptr;
		VkDeviceSize   readbackSize   = 0;
		int            textureDumpsRemaining = 8;
		bool           captureRequested = false;
		std::string    capturePath;

		std::string deviceName;
		std::string apiVersion;
	};
}
