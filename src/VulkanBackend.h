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

//// Dependencies

#include "VulkanApi.h"

#include <string>
#include <vector>

namespace scvk
{
	//// Types

	/**
	 * The Vulkan device, swapchain and fixed function emulation behind the driver.
	 *
	 * The driver turns the game's OpenGL-shaped calls into state; this turns that state
	 * into Vulkan work: pipelines keyed on vertex format, blend and depth state, push
	 * constants for everything that changes per draw, textures with their samplers, and
	 * the offscreen buffer regions the city view is saved into and restored from.
	 *
	 * The source is split by concern: VulkanBackend.cpp holds the device, swapchain,
	 * frames and captures, VulkanBackend_Draw.cpp the pipelines and draws,
	 * VulkanBackend_Textures.cpp the textures and samplers, and VulkanBackend_Regions.cpp
	 * the depth buffer and buffer regions.
	 *
	 * Every entry point is safe to call when initialisation failed. The game cannot be
	 * allowed to crash because a machine has no Vulkan driver, so a dead backend simply
	 * does nothing and says so once in the log.
	 */
	class VulkanBackend
	{
	private:
		//// Types

		/**
		 * What distinguishes one pipeline from another.
		 *
		 * Keyed on the game's vertex format id rather than the stride. Stride is not
		 * enough: V3F_C4UB_T2F and V3F_N3F are both 24 bytes but agree on nothing after
		 * the position, so keying on size alone silently reads normals as colours.
		 */
		struct PipelineKey
		{
			uint32_t            format;
			VkPrimitiveTopology topology;

			// Blending is baked into a Vulkan pipeline rather than being a command, so
			// every combination the game uses becomes its own pipeline. The factors are
			// the game's own enumeration.
			bool     isBlendEnabled;
			uint32_t sourceFactor;
			uint32_t destinationFactor;

			// Depth state is pipeline state in Vulkan too, so it joins the key rather
			// than being set per draw.
			bool     isDepthTestEnabled;
			bool     isDepthWriteEnabled;
			uint32_t depthComparison;

			// Whether colour is written. Vulkan makes this a pipeline field too, so a
			// pass drawn with writes off needs its own pipeline.
			bool     isColourWriteEnabled;

			// Whether the vertex copy carries a second coordinate set the format lacks,
			// which changes both the stride and the attributes.
			bool     hasAppendedCoordinateSet;

			bool operator==(PipelineKey const& other) const = default;
		};

		struct PipelineEntry
		{
			PipelineKey key;
			VkPipeline  pipeline;
		};

		/** Where a format's attributes live, decoded once per format. */
		struct VertexLayout
		{
			uint32_t stride       = 0;
			bool     hasColour    = false;
			uint32_t colourOffset = 0;

			// Coordinate sets, not components. Two means the geometry can feed a second
			// texture stage. The terrain carries both; buildings carry one and generate
			// the second stage's coordinates, which the copy then appends.
			uint32_t textureCoordinateSets      = 0;
			uint32_t textureCoordinateOffset[2] = { 0, 0 };

			// Whether the second set is the appended one rather than the format's own.
			bool hasAppendedCoordinateSet = false;
		};

		/**
		 * How one texture stage gets its coordinates.
		 *
		 * Either a coordinate set of the vertex or the eye-space position, then the two
		 * rows of the stage's texture matrix a 2D sample reads. For a generating stage
		 * the rows already include the modelview, so they apply to the object position.
		 */
		struct StageCoordinates
		{
			bool     isGenerated = false;
			uint32_t sourceSet   = 0;
			float    rows[8]     = { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f };
		};

		/**
		 * The fog every vertex of a draw reads, as three per-instance attributes.
		 *
		 * The dot product of the row with the object position is the eye-space depth in
		 * front of the camera. The parameters are the mode (0 off, 1 exponential, 2
		 * squared exponential, 3 linear), the density, and the scale and offset the linear
		 * equation reduces to.
		 */
		struct FogRecord
		{
			float distanceRow[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
			float parameters[4]  = { 0.0f, 1.0f, 0.0f, 1.0f };
			float colour[4]      = { 0.0f, 0.0f, 0.0f, 0.0f };
		};

		/** A stretch of a texture block, by offset and size. */
		struct MemoryRange
		{
			VkDeviceSize offset = 0;
			VkDeviceSize size   = 0;
		};

		/**
		 * One device-local allocation that textures are bound into side by side.
		 *
		 * Vulkan only promises 4096 allocations and some drivers stop there, while a city
		 * with custom content keeps over 10000 textures alive, so giving each texture an
		 * allocation of its own runs out. Blocks are kept until the device goes, like the
		 * arenas: the game's texture count levels off, and a freed range is reused.
		 */
		struct TextureBlock
		{
			VkDeviceMemory memory          = VK_NULL_HANDLE;
			uint32_t       memoryTypeIndex = 0;
			VkDeviceSize   usedBytes       = 0;

			// The unused ranges, sorted by offset with no two touching, so a range handed
			// back merges with its neighbours.
			std::vector<MemoryRange> freeRanges;

			// No free range is larger than this. Exact after a search that found no room,
			// raised when a range is handed back, so a full block is passed over without
			// walking its ranges.
			VkDeviceSize largestFreeBound = 0;
		};

		/**
		 * Where an image's memory lives: a range of a texture block, or an allocation of
		 * its own, which buffer regions and any texture too large for a block have.
		 */
		struct ImageMemory
		{
			VkDeviceMemory memory      = VK_NULL_HANDLE;
			VkDeviceSize   offset      = 0;
			VkDeviceSize   size        = 0;
			uint32_t       blockIndex  = 0;
			bool           isDedicated = true;

			// The stretch of the block taken, which starts before the offset when the
			// image needed aligning.
			MemoryRange    range;
		};

		/** A texture, its view, and the descriptor set that binds it. */
		struct Texture
		{
			VkImage         image      = VK_NULL_HANDLE;
			ImageMemory     memory;
			VkImageView     view       = VK_NULL_HANDLE;
			VkDescriptorSet descriptor = VK_NULL_HANDLE;
			VkFormat        format     = VK_FORMAT_UNDEFINED;
			uint32_t        width      = 0;
			uint32_t        height     = 0;
			uint32_t        levels     = 1;
			bool            isCompressed = false;
			bool            isLive       = false;

			// The texture pool its descriptor set came from, which is where it goes back.
			uint32_t descriptorPoolIndex = 0;

			// The highest level the game has actually filled, plus one. A declared level
			// that was never uploaded is undefined memory, and sampling it is
			// indistinguishable from sampling black.
			uint32_t uploadedLevels = 0;

			// How many uploads have reached any level, to tell a texture the game
			// rewrites from one it filled once.
			uint32_t uploadCount = 0;

			// The start of the top level of a texture of 16 texels or fewer, for the log:
			// one compressed block, or the first row of an uncompressed one.
			uint8_t  firstBytes[16] = {};
			uint32_t firstByteCount = 0;

			// The frame whose command buffer last sampled this texture. Uploads are
			// submitted at once while draws wait for the end of the frame, so an upload
			// in that same frame reaches draws that the game issued before it.
			uint64_t lastDrawnFrame = UINT64_MAX;
		};

		/**
		 * A deleted texture's or buffer region's objects, waiting for the GPU to finish
		 * with them. A region has no view or descriptor, so those stay null.
		 */
		struct RetiredImage
		{
			VkImage         image               = VK_NULL_HANDLE;
			ImageMemory     memory;
			VkImageView     view                = VK_NULL_HANDLE;
			VkDescriptorSet descriptor          = VK_NULL_HANDLE;
			uint32_t        descriptorPoolIndex = 0;
		};

		/** A descriptor pool holding only texture sets, and how many of them are out. */
		struct TexturePool
		{
			VkDescriptorPool pool     = VK_NULL_HANDLE;
			uint32_t         usedSets = 0;
		};

		/** A sampler and its set, keyed on the parameters that produced it. */
		struct SamplerEntry
		{
			uint32_t        key     = 0;
			VkSampler       sampler = VK_NULL_HANDLE;
			VkDescriptorSet set     = VK_NULL_HANDLE;
		};

		/**
		 * An offscreen copy of the colour or depth buffer.
		 *
		 * SimCity 4 draws the terrain and everything standing on it once, saves the
		 * result here, and then restores it every frame instead of drawing it again,
		 * redrawing only what moved. Without somewhere to save to the game never draws
		 * the city at all, so this is not an optimisation: it is the path the city view
		 * is on.
		 */
		struct BufferRegion
		{
			VkImage        image   = VK_NULL_HANDLE;
			VkDeviceMemory memory  = VK_NULL_HANDLE;
			VkFormat       format  = VK_FORMAT_UNDEFINED;
			uint32_t       width   = 0;
			uint32_t       height  = 0;
			bool           isDepth = false;
			bool           isLive  = false;

			// Regions start UNDEFINED and only hold anything once the game has saved into
			// them. Restoring from an empty one would be reading uninitialised memory, so
			// it is skipped instead.
			bool           hasContent = false;
		};

		/** One host-visible buffer of an arena, mapped for its whole life. */
		struct ArenaBlock
		{
			VkBuffer       buffer = VK_NULL_HANDLE;
			VkDeviceMemory memory = VK_NULL_HANDLE;
			void*          mapped = nullptr;
		};

		/**
		 * Per-frame vertex or index data, bump allocated and rewound each frame. The draw
		 * is recorded now and runs later, so the bytes have to stay put until the submit
		 * completes.
		 *
		 * Split into blocks and grown a block at a time. A single fixed buffer overflowed
		 * at the widest zoom, where one frame redraws the whole city, and every draw past
		 * that point was dropped, which left holes in the saved scene. A frame that fills
		 * every block is submitted part way and the arena reused.
		 */
		struct Arena
		{
			char const*             name          = "";
			VkBufferUsageFlags      usage         = 0;
			VkDeviceSize            blockSize     = 0;
			size_t                  maximumBlocks = 0;
			std::vector<ArenaBlock> blocks;
			size_t                  currentBlock  = 0;
			VkDeviceSize            usedBytes     = 0;
		};

		//// Constants

		// Depth is read and written in both fragment test stages, early when the shader
		// cannot discard and late when it can, so a dependency on depth names both.
		static constexpr VkPipelineStageFlags DEPTH_STAGES = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;

		// Where a frame waits for its swapchain image, and so where the first barrier on
		// that image has to start for the two to form a chain. A frame's first use of the
		// image is either a copy or a draw.
		static constexpr VkPipelineStageFlags ACQUIRE_STAGES = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

		// Distinct filter and wrap combinations; the game uses a handful.
		static constexpr size_t MAXIMUM_SAMPLERS = 64;

		//// State

		// The instance and the device
		VkDebugUtilsMessengerEXT debugMessenger = VK_NULL_HANDLE;
		VkInstance               instance       = VK_NULL_HANDLE;
		VkPhysicalDevice         physicalDevice = VK_NULL_HANDLE;
		VkDevice                 device         = VK_NULL_HANDLE;
		VkQueue                  queue          = VK_NULL_HANDLE;
		uint32_t                 queueFamily    = UINT32_MAX;
		std::string              deviceName;
		std::string              apiVersion;
		bool                     isDead         = false;

		// Vulkan only promises 4096 allocations, so every one is counted against the
		// device's own limit.
		uint32_t maximumMemoryAllocations = 0;
		uint32_t liveMemoryAllocations    = 0;

		// The window, the swapchain and what renders into it
		void*                      windowHandle    = nullptr;
		VkSurfaceKHR               surface         = VK_NULL_HANDLE;
		VkSwapchainKHR             swapchain       = VK_NULL_HANDLE;
		VkFormat                   swapchainFormat = VK_FORMAT_UNDEFINED;
		VkExtent2D                 swapchainExtent{};
		std::vector<VkImage>       swapchainImages;
		std::vector<VkImageView>   swapchainImageViews;
		std::vector<VkFramebuffer> framebuffers;
		VkRenderPass               renderPass      = VK_NULL_HANDLE;
		uint32_t                   imageIndex      = 0;

		// The frame being recorded. One frame is in flight at a time.
		VkCommandPool   commandPool             = VK_NULL_HANDLE;
		VkCommandBuffer commandBuffer           = VK_NULL_HANDLE;
		VkSemaphore     imageAvailableSemaphore = VK_NULL_HANDLE;
		VkSemaphore     renderFinishedSemaphore = VK_NULL_HANDLE;
		VkFence         frameFence              = VK_NULL_HANDLE;
		VkImageLayout   currentLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
		bool            isFrameActive           = false;
		bool            isRenderPassActive      = false;
		uint64_t        presentedFrames         = 0;

		// Whether part of the frame has already been submitted, which consumed the wait for
		// its swapchain image. A readback submits the frame early to wait for it.
		bool            hasSubmittedImageWait   = false;

		// The performance counter at the last heartbeat, for the frame rate it reports.
		int64_t lastHeartbeatTicks = 0;

		// The gaps between presents since the last heartbeat. A stall shows up here even
		// when the average rate hides it, which is what a texture reload looks like.
		int64_t  ticksPerSecond    = 0;
		int64_t  lastPresentTicks  = 0;
		int64_t  slowestFrameTicks = 0;
		uint32_t slowFrames        = 0;

		// Reused when a Flush arrives with no frame started, so the swapchain keeps
		// cycling instead of stalling.
		float lastClearColour[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

		// Staging memory for pixel uploads. Commands are recorded now and executed later,
		// so the bytes have to stay put until the submit completes. Each blit takes the
		// next slice and the whole thing is rewound when a frame begins.
		VkBuffer       stagingBuffer = VK_NULL_HANDLE;
		VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
		VkDeviceSize   stagingSize   = 0;
		VkDeviceSize   stagingUsed   = 0;
		void*          stagingMapped = nullptr;

		// Per-frame geometry, and the static indices turning consecutive quads into
		// triangle pairs.
		Arena          vertexArena;
		Arena          indexArena;
		VkBuffer       quadIndexBuffer = VK_NULL_HANDLE;
		VkDeviceMemory quadIndexMemory = VK_NULL_HANDLE;
		uint32_t       quadCapacity    = 0;

		// Shaders and pipelines. The vertex modules are indexed by hasColour * 3 +
		// textureCoordinateSets.
		VkShaderModule             vertexModules[6] = {};
		VkShaderModule             fragmentModule   = VK_NULL_HANDLE;
		VkPipelineLayout           pipelineLayout   = VK_NULL_HANDLE;
		std::vector<PipelineEntry> pipelines;

		// The combined transform, already corrected into Vulkan clip space.
		float transform[16] = {
			1, 0, 0, 0,
			0, 1, 0, 0,
			0, 0, 1, 0,
			0, 0, 0, 1,
		};

		// The viewport in the game's coordinates, converted when a draw applies it.
		// Negative width means "not set yet", so the full window is used.
		int32_t viewportX             = 0;
		int32_t viewportY             = 0;
		int32_t viewportWidth         = -1;
		int32_t viewportHeight        = -1;
		int     viewportLogsRemaining = 12;
		int32_t loggedViewport[4]     = { -1, -1, -1, -1 };

		// Sent to the shader alongside the transform: alpha comparison, reference, the
		// texture environment mode, and which of the shader's paths the draw takes.
		float fragmentState[4] = { -1.0f, 0.0f, 0.0f, 0.0f };

		// Pipeline state, held until a draw selects a pipeline with it. The numbers are
		// the game's own enumerations: less for depth, one and zero for blending.
		bool     isDepthTestEnabled     = false;
		bool     isDepthWriteEnabled    = true;
		uint32_t depthComparison        = 1;
		bool     isBlendEnabled         = false;
		uint32_t blendSourceFactor      = 1;
		uint32_t blendDestinationFactor = 0;
		bool     isColourWriteEnabled   = true;

		// The texture environment and lighting the fragment and vertex stages read. The
		// mode holds only the three the single stage path knows, so whether the first
		// stage asked for its combiner network is kept beside it.
		uint32_t textureEnvironmentMode = 1;
		bool     isFirstStageCombining  = false;
		bool     isColourFromVertex     = true;
		bool     isAlphaFromVertex      = true;
		float    sceneTint[4]           = { 1.0f, 1.0f, 1.0f, 1.0f };

		// The combiner network and the environment colour a combiner may name. The second
		// stage needs geometry carrying two coordinate sets, which in practice means the
		// terrain and the building shadows drawn over it. Without the second stage the
		// shadows run the first stage's network alone.
		bool     isStageEnabled[2] = { false, false };
		uint32_t combinerState[4]  = {};
		float    constantColour[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

		// Where each stage's texture coordinates come from. The single stage paths hand
		// the first stage's rows to the shader in the push constant space the combiner
		// takes on the two stage path, so that path has them written into its vertex copy
		// instead.
		StageCoordinates stageCoordinates[2] = { StageCoordinates{}, StageCoordinates{ false, 1, { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f } } };

		// The fog, which reaches the vertex stage through a second vertex binding because
		// the push constant block is full. The record is written into the vertex arena
		// when it changes and every draw until the next change binds the same copy; a
		// null buffer means there is no current copy.
		FogRecord    fogRecord;
		bool         isFogEnabled    = false;
		VkBuffer     fogRecordBuffer = VK_NULL_HANDLE;
		VkDeviceSize fogRecordOffset = 0;

		// Diagnostics that replace every colour on screen.
		bool shouldShowPassColours = false;
		int  debugChannel          = -1;

		// The depth attachment. The layout is pending when the image has just been
		// created and is still UNDEFINED; whichever comes first, the next render pass or
		// the game's next depth clear, moves it.
		VkImage        depthImage            = VK_NULL_HANDLE;
		VkDeviceMemory depthMemory           = VK_NULL_HANDLE;
		VkImageView    depthView             = VK_NULL_HANDLE;
		VkFormat       depthFormat           = VK_FORMAT_UNDEFINED;
		bool           isDepthLayoutPending  = false;

		// A copy of each frame as it is presented, sized with the swapchain. The game
		// reads the screen back between frames for a photo, expecting the frame it last
		// showed, and a presented swapchain image cannot be read.
		BufferRegion   lastFrame;

		// Descriptor layouts: one set per texture, and one per sampler.
		VkDescriptorSetLayout imageSetLayout   = VK_NULL_HANDLE;
		VkDescriptorSetLayout samplerSetLayout = VK_NULL_HANDLE;
		VkDescriptorPool      descriptorPool   = VK_NULL_HANDLE;

		// Texture sets come from a chain of pools that grows when every pool is full,
		// because a city with custom content keeps thousands of textures alive at once.
		std::vector<TexturePool> texturePools;
		uint32_t                 textureSetsPerPool = 0;

		// Index 0 is a 1x1 white texture, so an untextured draw multiplies by one instead
		// of needing its own shader and pipeline.
		std::vector<Texture>        textures;
		std::vector<TextureBlock>   textureBlocks;
		std::vector<RetiredImage>   retiredImages;
		std::vector<SamplerEntry>   samplers;
		uint32_t                    currentTexture  = 0;
		uint32_t                    currentTexture1 = 0;

		// Each stage's magnification filter, minification filter, wrap S and wrap T, in the
		// game's own numbering. They belong to the stage, so binding another texture there
		// leaves them alone. Linear with repeat until the game says otherwise.
		uint32_t                    stageParameters[2][4] = { { 1, 1, 3, 3 }, { 1, 1, 3, 3 } };

		VkCommandBuffer             uploadCommandBuffer = VK_NULL_HANDLE;
		VkFence                     uploadFence         = VK_NULL_HANDLE;

		// Texture uses that OpenGL would order differently from us, counted so the log
		// says whether they happen at all.
		uint64_t drawsBeforeUpload     = 0;
		uint64_t uploadsAfterDraw      = 0;
		int      hazardNotesRemaining  = 40;
		int      textureDumpsRemaining = 8;

		// Texture traffic since the last heartbeat, which says how often the game reloads
		// textures and so whether its texture cache (texBindMaxFree) is big enough.
		uint32_t     texturesCreated    = 0;
		uint32_t     texturesDestroyed  = 0;
		uint32_t     textureUploads     = 0;
		VkDeviceSize textureUploadBytes = 0;
		int64_t      textureWorkTicks   = 0;
		int64_t      textureMemoryTicks = 0;

		// Handle n is index n - 1, matching what the game is handed back, and leaving 0
		// free to mean failure.
		std::vector<BufferRegion> bufferRegions;

		// Readback for the captures. Allocated on first use and kept, since captures come
		// in small numbers and the buffer is large.
		VkBuffer       readbackBuffer = VK_NULL_HANDLE;
		VkDeviceMemory readbackMemory = VK_NULL_HANDLE;
		void*          readbackMapped = nullptr;
		VkDeviceSize   readbackSize   = 0;

		bool        isCaptureRequested       = false;
		std::string capturePath;
		bool        isRegionCaptureRequested = false;
		bool        isRegionCaptureDepth     = false;
		std::string regionCapturePath;

		//// Private Functions

		// Device and swapchain, in VulkanBackend.cpp

		/** Logs once per distinct failure, then goes quiet. */
		void Fail(char const* what, VkResult result);

		bool PickPhysicalDevice(void);
		bool CreateLogicalDevice(void);
		bool CreateFrameResources(void);
		bool CreateStagingBuffer(VkDeviceSize size);
		bool CreateSwapchain(uint32_t width, uint32_t height);
		bool CreateRenderPass(void);
		bool CreateFramebuffers(void);
		void DestroyFramebuffers(void);
		void DestroySwapchain(void);

		/** Makes the swapchain again once the window has area, or returns false while it has none. */
		bool RestoreSwapchain(void);

		/** Destroys everything that belongs to the device and the window, keeping the instance. */
		void DestroyDevice(void);

		bool FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags properties, uint32_t& outIndex) const;

		/** vkAllocateMemory, counting the allocation against the device's limit. */
		VkResult AllocateDeviceMemory(VkMemoryAllocateInfo const& information, VkDeviceMemory& outMemory);

		/** vkFreeMemory for memory from AllocateDeviceMemory, clearing the handle. Does nothing for a null one. */
		void FreeDeviceMemory(VkDeviceMemory& memory);

		bool CreateHostBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& outBuffer, VkDeviceMemory& outMemory, void*& outMapped);

		/** The swapchain extent as signed numbers, for arithmetic against the game's signed rectangles. */
		int32_t SwapchainWidth(void) const;
		int32_t SwapchainHeight(void) const;

		/** Starts a frame if one is not already in progress. */
		bool EnsureFrame(void);

		/** Submits what the frame has recorded so far, waits for it, and carries on recording the same frame. */
		bool SubmitFrameSoFar(void);

		/** Barriers the swapchain image into a layout, tracking where it was. */
		void TransitionTo(VkImageLayout newLayout);

		/** What a layout implies about access and pipeline stage. */
		static void LayoutAccess(VkImageLayout layout, VkAccessFlags& outAccess, VkPipelineStageFlags& outStages);

		void BeginRenderPassIfNeeded(void);
		void EndRenderPassIfActive(void);
		void ApplyViewport(void);

		/**
		 * The viewport in framebuffer coordinates, clamped to the swapchain. Returns
		 * whether a sub-viewport is in force, which is when the game's OpenGL driver
		 * enables its scissor test with the same rectangle.
		 */
		bool ViewportRectangle(VkRect2D& outRectangle) const;

		/** Clips a copy's destination to the scissor, moving the source with it. Returns false when nothing is left to copy. */
		bool ClipToScissor(int32_t& sourceX, int32_t& sourceY, int32_t& destinationX, int32_t& destinationY, int32_t& width, int32_t& height) const;

		/** Makes the readback buffer at least this large. */
		bool EnsureReadbackBuffer(VkDeviceSize size);
		void DestroyReadbackBuffer(void);

		/** Records the copy of a saved region for a requested region capture. */
		bool RecordRegionCapture(uint32_t& outWidth, uint32_t& outHeight);

		/** Records the copy of the swapchain image for a requested frame capture. */
		bool RecordFrameCapture(void);

		/** Writes whichever capture this frame recorded, once the GPU has finished it. */
		void WriteCapture(bool isRegion, uint32_t regionWidth, uint32_t regionHeight);

		/** The periodic line saying the swapchain is still presenting. */
		void LogHeartbeat(void);

		/** Writes BGRA pixels as a 32 bit BMP. */
		static bool WriteBmp(char const* path, uint8_t const* pixels, uint32_t width, uint32_t height, uint32_t rowPitch);

		// Pipelines and draws, in VulkanBackend_Draw.cpp

		bool CreateShaderModule(uint32_t const* code, size_t wordCount, VkShaderModule& outModule);
		bool CreateShaderModules(void);
		bool CreatePipelineLayout(void);
		bool CreateGeometryBuffers(void);
		VkPipeline GetPipeline(PipelineKey const& key);
		void DestroyPipelines(void);

		/** Where a format's attributes live, from the game's packed encoding. */
		static VertexLayout DecodeVertexLayout(uint32_t gdVertexFormat);

		/** A layout with a second coordinate set added after the format's own attributes. */
		static VertexLayout AppendCoordinateSet(VertexLayout layout);

		/** The layout the vertex copy takes: the format's own, widened when the second stage needs a set it lacks. */
		VertexLayout DecodeDrawLayout(VertexLayout const& sourceLayout) const;

		/** The game's primitive numbering to a Vulkan topology. */
		static bool MapTopology(uint32_t gdPrimitiveType, VkPrimitiveTopology& outTopology, bool& outIsQuadList);

		/** Adds one block to an arena. */
		bool ArenaAddBlock(Arena& arena);

		/** Reserves space, moving to the next block, or adding one, when this one is full. */
		bool ArenaAllocate(Arena& arena, VkDeviceSize bytes, VkDeviceSize alignment, VkBuffer& outBuffer, VkDeviceSize& outOffset, uint8_t*& outAddress);

		/** Whether an arena can hand out this much, in one piece, before it is rewound. */
		static bool ArenaHasRoom(Arena const& arena, VkDeviceSize bytes, VkDeviceSize alignment);

		/** Rewinds an arena to its first block. */
		static void ArenaRewind(Arena& arena);

		void DestroyArena(Arena& arena);

		/** Makes room for one draw's vertices and indices, submitting the frame so far when the arenas are full. */
		bool ReserveDrawSpace(VkDeviceSize vertexBytes, VkDeviceSize indexBytes);

		/** Copies a vertex range into the per-frame arena, with the coordinates the draw samples. */
		bool UploadVertices(void const* vertices, uint32_t firstVertex, uint32_t vertexCount, VertexLayout const& sourceLayout, VertexLayout const& drawLayout, VkBuffer& outBuffer, VkDeviceSize& outOffset);

		/** Whether the second stage takes part in the draw. */
		bool IsTwoStageDraw(uint32_t textureCoordinateSets) const;

		/** Whether the draw runs the combiner network, on both stages or on the first alone. */
		bool IsCombinerDraw(uint32_t textureCoordinateSets) const;

		/** Whether a stage's coordinates differ from the vertex set of its own number. */
		bool IsStageTransformed(uint32_t stage) const;

		/** Writes each stage's final coordinates into its own set of the vertex copy. */
		void WriteStageCoordinates(uint8_t* destination, uint8_t const* source, uint32_t vertexCount, VertexLayout const& sourceLayout, VertexLayout const& drawLayout) const;

		/** Everything a draw needs bound, shared by the indexed and plain paths. */
		bool BindDrawState(uint32_t gdVertexFormat, VkPrimitiveTopology topology, VkBuffer vertexBuffer, VkDeviceSize vertexOffset, VertexLayout const& drawLayout);

		/** Pushes the per-draw constants, adjusted for a disabled first stage and the diagnostics. */
		void PushDrawConstants(bool isTwoStage, bool isCombining);

		/** Binds the textures and sampler of both stages, applying their parameters. */
		void BindTextures(bool isTwoStage);

		/** Changes the fog record, so the next draw writes a fresh copy of it. */
		void UpdateFogRecord(FogRecord const& record);

		/** The current fog record's copy in the vertex arena, written first if there is none. */
		bool GetFogRecordCopy(VkBuffer& outBuffer, VkDeviceSize& outOffset);

		// Textures, in VulkanBackend_Textures.cpp

		bool CreateDescriptorResources(void);
		bool CreateDefaultTexture(void);
		void DestroyTextures(void);

		/** Finds memory for a texture image, in a texture block unless it is larger than one. */
		bool AllocateTextureMemory(VkMemoryRequirements const& requirements, ImageMemory& outMemory);

		/** Takes the first free range of a block that fits, aligned. Returns false when none does. */
		static bool TakeBlockRange(TextureBlock& block, VkDeviceSize size, VkDeviceSize alignment, MemoryRange& outRange, VkDeviceSize& outOffset);

		/** Gives a range back to its block, merging it with the free ranges either side. */
		static void ReturnBlockRange(TextureBlock& block, MemoryRange range);

		/** Gives an image's memory back, to its block or to the device, and clears it. */
		void ReleaseImageMemory(ImageMemory& memory);

		/** Destroys everything retired since the last frame. */
		void FlushRetiredImages(void);

		/** The sampler for one set of filter and wrap parameters. */
		VkDescriptorSet GetSamplerSet(uint32_t const parameters[4]);

		/** A descriptor set for a new texture, from the first texture pool with room. */
		bool AllocateTextureSet(VkDescriptorSet& outSet, uint32_t& outPoolIndex);

		/** Records a draw sampling a texture, and reports the hazards counted above. */
		void NoteTextureUse(uint32_t handle);

		/** Converts one level's texels into the image's own layout. Returns false for a format it cannot read. */
		bool StageTexels(Texture const& texture, uint32_t width, uint32_t height, uint32_t gdFormat, uint32_t gdType, uint32_t rowLength, void const* pixels, std::vector<uint8_t>& outStaged);

		/** The heartbeat's line on live textures and the traffic since the last one. */
		void LogTextureTraffic(void);

		/** Writes an uploaded interface texture to a file, a handful of times per session. */
		void DumpUploadedTexture(Texture const& texture, uint32_t handle, uint32_t width, uint32_t height, uint32_t gdFormat, uint32_t gdType, uint32_t rowLength, void const* pixels);

		/** Starts recording into the upload command buffer. */
		void BeginUploadCommands(void);

		/** Submits the upload command buffer and waits for it. */
		void SubmitUploadCommands(void);

		// Depth and buffer regions, in VulkanBackend_Regions.cpp

		bool CreateDepthResources(void);
		void DestroyDepthResources(void);

		/** Barriers the depth image, which is not tracked the way the swapchain image is. */
		void BarrierDepthImage(VkImageLayout oldLayout, VkImageLayout newLayout, VkAccessFlags sourceAccess, VkAccessFlags destinationAccess, VkPipelineStageFlags sourceStages, VkPipelineStageFlags destinationStages);

		/** The depth image's layout outside a copy: UNDEFINED until something first moves it. */
		VkImageLayout DepthRestingLayout(void) const;

		/** Barriers a region image, which is not the tracked swapchain one. */
		void TransitionRegion(BufferRegion const& region, VkImageLayout oldLayout, VkImageLayout newLayout);

		/** Clamps a copy between the window and a region to both images. Returns false when either origin is negative. */
		bool ClampRegionCopy(BufferRegion const& region, int32_t regionX, int32_t regionY, int32_t screenX, int32_t screenY, int32_t& width, int32_t& height) const;

		/** Creates a region's image and memory at the window's size, without giving it a handle. */
		bool AllocateRegionImage(bool isDepth, BufferRegion& outRegion);

		bool CreateLastFrame(void);
		void DestroyLastFrame(void);

		/** Records the copy of the frame into lastFrame, as the last thing before presenting. */
		void SaveLastFrame(void);

	public:
		//// Public API

		VulkanBackend(void);
		~VulkanBackend(void);

		VulkanBackend(VulkanBackend const&) = delete;
		VulkanBackend& operator=(VulkanBackend const&) = delete;

		// Setup and teardown

		/** Loads Vulkan, creates the instance, and picks a physical device. Does nothing once done. */
		bool CreateInstance(void);

		/**
		 * Creates the surface, device and swapchain for a window. Calling it again for a
		 * new window first destroys what the previous call made.
		 */
		bool CreateSurfaceAndDevice(void* newWindowHandle, uint32_t width, uint32_t height);

		void Destroy(void);

		/** Whether frames can be drawn, rebuilding a swapchain lost while the window had no area. */
		bool IsReady(void);

		/** Description of the selected GPU, for GetDriverInfo. */
		std::string const& DeviceName(void) const { return deviceName; }
		std::string const& ApiVersion(void) const { return apiVersion; }

		// Frames

		void Clear(float red, float green, float blue, float alpha);

		/**
		 * Copies tightly packed BGRA8 pixels into the current image.
		 *
		 * The game hands over BGRA8 and the Windows swapchain is natively B8G8R8A8_UNORM,
		 * so the pixels go straight in with no conversion. sourceWidth is the row stride
		 * of the source in pixels, which matters when the destination is clipped: the
		 * rows still have to be strided by the full source width.
		 */
		void BlitPixels(int32_t destinationX, int32_t destinationY, uint32_t width, uint32_t height, uint32_t sourceWidth, void const* pixels);

		/**
		 * Copies a rectangle of the screen into tightly packed BGRA8 rows, top row first,
		 * with every alpha opaque.
		 *
		 * Mid-frame that is what the frame has drawn so far, and the commands recorded so
		 * far are submitted early to wait for them. Between frames it is the frame last
		 * presented. The rectangle is in window coordinates measured from the top, and
		 * has to lie inside the window.
		 */
		bool ReadFramePixels(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint8_t* outPixels);

		/** The window's size, which bounds what ReadFramePixels can read. */
		uint32_t FrameWidth(void) const { return swapchainExtent.width; }
		uint32_t FrameHeight(void) const { return swapchainExtent.height; }

		/** Ends the frame, submits, and presents. */
		void Present(void);

		/**
		 * Writes the next presented frame to a file, as a 32 bit BMP.
		 *
		 * Captured from the swapchain image rather than from the screen. That is the only
		 * way to be sure the picture is what scvk drew: grabbing the desktop needs the
		 * window focused, cannot see a Vulkan surface reliably, and captures whatever
		 * else happens to be on screen.
		 *
		 * BMP because its 32 bit layout is already BGRA, matching the swapchain exactly,
		 * so no encoder and no conversion are needed.
		 */
		void RequestCapture(char const* path);

		/**
		 * Writes the saved colour buffer region to a file after the next present. That is
		 * the copy of the scene the game restores every frame, so it answers whether
		 * something wrong was saved into it or only drawn over it afterwards.
		 *
		 * For depth it reads the saved depth region instead, written as a width and
		 * height followed by raw 32-bit floats.
		 */
		void RequestRegionCapture(char const* path, bool isDepth = false);

		// Draw state

		/**
		 * Sets the transform for subsequent draws.
		 *
		 * Takes the game's combined projection times modelview in OpenGL column-major
		 * convention. The correction into Vulkan clip space is applied here rather than
		 * by the caller, because it is a property of the API rather than of the game.
		 */
		void SetTransform(float const* openGlModelViewProjection);

		/**
		 * Sets the viewport, in the game's OpenGL convention.
		 *
		 * The game renders parts of the interface into sub-rectangles and pairs each with
		 * a projection matched to that rectangle, so ignoring this stretches a small
		 * region across the whole window.
		 *
		 * y is measured from the bottom, as OpenGL does it; Vulkan measures from the top,
		 * and the flip happens here.
		 */
		void SetViewport(int32_t x, int32_t y, int32_t width, int32_t height);

		/** Resets the viewport to the whole window. */
		void SetFullViewport(void);

		/**
		 * Sets the blend state for subsequent draws.
		 *
		 * Factors are the game's own enumeration, which matches OpenGL's order. Blending
		 * is part of a Vulkan pipeline rather than a command, so changing it selects a
		 * different pipeline.
		 */
		void SetBlendState(bool isEnabled, uint32_t sourceFactor, uint32_t destinationFactor);

		/**
		 * Sets the alpha test for subsequent draws.
		 *
		 * A negative comparison disables it. Applied in the fragment shader, since Vulkan
		 * has no fixed function alpha test.
		 */
		void SetAlphaTest(int comparison, float reference);

		/** Selects the texture environment: 0 replace, 1 modulate, 2 decal. */
		void SetTextureEnvironmentMode(uint32_t mode);

		/** Whether colour is written at all. Pipeline state in Vulkan. */
		void SetColourWrite(bool isEnabled);

		/** Sets depth testing, writing and the comparison, all pipeline state. */
		void SetDepthState(bool isTestEnabled, bool isWriteEnabled, uint32_t comparison);

		/** Clears the depth attachment to the given value. */
		void ClearDepth(float depth);

		/**
		 * The ambient light and alpha multiplier applied to every lit draw, and whether the
		 * vertex colour stands in for the material's colour and alpha.
		 */
		void SetSceneTint(float red, float green, float blue, float alpha, bool isColourFromVertexColour, bool isAlphaFromVertexColour);

		/** The environment colour a combiner may name as a source. */
		void SetConstantColour(float red, float green, float blue, float alpha);

		/**
		 * Sets one stage's combiner, already packed the way the shader reads it.
		 *
		 * Two words per stage, one for colour and one for alpha, each holding the combine
		 * mode, three source and operand pairs, and the output scale.
		 */
		void SetCombinerState(uint32_t stage, uint32_t packedRgb, uint32_t packedAlpha);

		/**
		 * Sets where a stage's texture coordinates come from.
		 *
		 * Either vertex coordinate set sourceSet or the eye-space position, transformed
		 * by the two rows of the stage's texture matrix a 2D sample reads. Generated rows
		 * arrive already multiplied through the modelview. OpenGL transforms every
		 * texture coordinate of a stage by its matrix, not only generated ones.
		 */
		void SetStageCoordinates(uint32_t stage, bool isGenerated, uint32_t sourceSet, float const* rowS, float const* rowT);

		/**
		 * Sets the fog for subsequent draws.
		 *
		 * The mode is the game's own enumeration, 0 exponential, 1 squared exponential and
		 * 2 linear, which follows OpenGL's EXP, EXP2 and LINEAR. The colour is four floats.
		 */
		void SetFog(bool isEnabled, uint32_t gdMode, float density, float start, float end, float const* colour);

		/**
		 * Sets the row whose dot product with an object position is its distance in
		 * front of the camera, for the fog. Ignored while the fog is off.
		 */
		void SetFogDistanceRow(float const* row);

		/** Replaces draw colours with a flat colour per blend configuration. */
		void SetDebugPassColours(bool isEnabled);

		/**
		 * Replaces every draw's colour with one of its inputs: 0 the texture colour, 1
		 * the texture alpha, 2 the vertex colour, 3 the vertex alpha. Negative leaves the
		 * picture alone.
		 */
		void SetDebugChannel(int channel);

		// Draws

		/**
		 * Draws from client memory.
		 *
		 * The interface hands over a plain pointer and a stride, so the data is copied
		 * into a per-frame buffer before each draw. Quads have no Vulkan equivalent and
		 * are drawn indexed as triangle pairs.
		 */
		void DrawVertices(uint32_t gdPrimitiveType, uint32_t gdVertexFormat, void const* vertices, uint32_t firstVertex, uint32_t vertexCount);

		/**
		 * Draws from client memory through a client index array.
		 *
		 * This is the path the city view is on: the terrain and everything standing on it
		 * are indexed meshes, and a session inside a city issues well over a million of
		 * these against a few hundred thousand of the unindexed kind.
		 */
		void DrawIndexedVertices(uint32_t gdPrimitiveType, uint32_t gdVertexFormat, void const* vertices, void const* indices, uint32_t indexCount, bool isIndex32Bit);

		// Textures

		/**
		 * Creates a texture and returns a handle, or 0 on failure.
		 *
		 * gdInternalFormat is the game's own enumeration: 0 to 4 are uncompressed and 5
		 * to 7 are DXT1, DXT3 and DXT5.
		 */
		uint32_t CreateTexture(uint32_t gdInternalFormat, uint32_t width, uint32_t height, uint32_t levels);

		/** Uploads one level, or a rectangle of one level. */
		void UploadTextureLevel(uint32_t handle, uint32_t level, int32_t offsetX, int32_t offsetY, uint32_t width, uint32_t height, uint32_t gdFormat, uint32_t gdType, uint32_t rowLength, void const* pixels);

		/**
		 * Sets one filter or wrap parameter on a stage.
		 *
		 * The type is 0 for the magnification filter, 1 the minification filter, 2 wrap
		 * S and 3 wrap T. The stage keeps it, as a Direct3D texture stage does, and every
		 * draw samples whatever texture is bound there with it.
		 */
		void SetStageParameter(uint32_t stage, uint32_t parameterType, uint32_t value);

		/** A stage's filter and wrap parameters, for the log. */
		void GetStageParameters(uint32_t stage, uint32_t outParameters[4]) const;

		/** Selects the texture used by subsequent draws. 0 means untextured. */
		void SetTexture(uint32_t handle);

		/** Selects the texture on the second stage. 0 disables that stage. */
		void SetTexture1(uint32_t handle);

		/** Switches texturing on or off for one stage. */
		void SetTextureStageEnabled(uint32_t stage, bool isEnabled);

		void DestroyTexture(uint32_t handle);

		/** Describes a texture in the log, for working out why one samples wrong. */
		void LogTextureInformation(uint32_t handle, char const* reason);

		/** A live texture's size, levels and upload count, or false when there is none. */
		bool DescribeTexture(uint32_t handle, uint32_t& outWidth, uint32_t& outHeight, uint32_t& outLevels, uint32_t& outUploadedLevels, uint32_t& outUploadCount) const;

		/** Both hazard counts together, so a caller can tell whether any happened. */
		uint64_t TextureHazardCount(void) const { return drawsBeforeUpload + uploadsAfterDraw; }

		// Buffer regions

		/**
		 * Allocates an offscreen copy of the colour or depth buffer.
		 *
		 * Returns a handle, or 0 if one could not be made. The game asks for the back
		 * colour buffer and the depth buffer.
		 */
		uint32_t CreateBufferRegion(bool isDepth);

		/** Copies a rectangle of the framebuffer into a region. */
		bool SaveBufferRegion(uint32_t handle, int32_t regionX, int32_t regionY, int32_t width, int32_t height, int32_t screenX, int32_t screenY);

		/** Copies a rectangle of a region back into the framebuffer. */
		bool RestoreBufferRegion(uint32_t handle, int32_t regionX, int32_t regionY, int32_t width, int32_t height, int32_t screenX, int32_t screenY);

		bool IsBufferRegion(uint32_t handle) const;
		void DestroyBufferRegion(uint32_t handle);
		void DestroyAllBufferRegions(void);
	};
}
