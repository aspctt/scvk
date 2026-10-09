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
#include <unordered_map>
#include <vector>

namespace scvk
{
	//// Types

	/**
	 * What a plugin observing the frame is handed, and what the frame callback API passes
	 * on. Valid only for the duration of the call that produced it.
	 */
	struct ExternalFrame
	{
		VkInstance       instance       = VK_NULL_HANDLE;
		VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
		VkDevice         device         = VK_NULL_HANDLE;
		VkQueue          queue          = VK_NULL_HANDLE;
		uint32_t         queueFamily    = 0;

		// Recording, outside any render pass. The back buffer is in
		// COLOR_ATTACHMENT_OPTIMAL and has to be left that way.
		VkCommandBuffer  commandBuffer  = VK_NULL_HANDLE;
		VkImage          backBuffer     = VK_NULL_HANDLE;
		VkImageView      backBufferView = VK_NULL_HANDLE;
		VkFormat         format         = VK_FORMAT_UNDEFINED;
		uint32_t         width          = 0;
		uint32_t         height         = 0;
	};

	/** How many lights the lighting extension has, and the lit vertex carries. */
	constexpr uint32_t LIT_LIGHT_COUNT = 8;

	/**
	 * A vertex of a draw the lighting extension lights per pixel.
	 *
	 * Drawn under LIT_VERTEX_FORMAT, which stands in for the game's vertex format. The
	 * colour holds the lighting that does not depend on direction, as BGRA like the game's;
	 * the rest lets the fragment stage add each light's diffuse term per pixel. Disabled
	 * lights have a black colour. See cVKDriver::LightVertices.
	 */
	struct LitVertex
	{
		float   position[3];
		uint8_t colour[4];
		float   coordinates[2][2];

		// Eye space, normalised; zero for a vertex without a normal
		float normal[3];

		// Eye space, from the vertex to each light; the light's direction for a directional one
		float toLight[LIT_LIGHT_COUNT][3];

		// Each light's diffuse colour times the diffuse material, RGBA
		uint8_t lightColour[LIT_LIGHT_COUNT][4];
	};

	/** The vertex format a LitVertex draw is made under, outside the game's packed ones. */
	constexpr uint32_t LIT_VERTEX_FORMAT = 0x7f4c4954u;

	/** A vertex of a shadow caster: world or model position, then its texture coordinate. */
	struct ShadowVertex
	{
		float position[3];
		float uv[2];
	};

	/** One draw into the shadow map. */
	/** How a caster's indices are drawn into the shadow map. */
	enum ShadowCasterTopology : uint32_t
	{
		SHADOW_CASTER_TRIANGLES = 0,
		SHADOW_CASTER_LINES     = 1,
		SHADOW_CASTER_POINTS    = 2,
	};

	struct ShadowCasterDraw
	{
		// Into the vertices and indices handed over with the batch.
		uint32_t firstIndex   = 0;
		uint32_t indexCount   = 0;
		int32_t  vertexOffset = 0;

		// The texture whose alpha cuts the silhouette, 0 for a solid caster, and the
		// filter and wrap to sample it with, in the game's numbering.
		uint32_t texture              = 0;
		uint32_t samplerParameters[4] = { 1, 1, 2, 2 };

		// The push constants: a light matrix, the two texture rows a 2D sample reads, then
		// four floats whose meaning depends on the pipeline. See shadow.frag.
		float constants[32] = {};

		// Registry casters project their texture from the world position instead of
		// reading the vertex coordinates. See the shadow shaders.
		bool isRegistry = false;

		// Lists of triangles, lines or points, as the game drew the caster. Registry
		// casters are always triangles.
		uint32_t topology = SHADOW_CASTER_TRIANGLES;
	};

	/** What the shadow composite reads, laid out as the composite shader declares its uniform block. */
	struct ShadowCompositeConstants
	{
		float lightMatrix[16];
		float material[4];
		float projection0[4];
		float projection1[4];
		float viewport[4];
		float tone[4];
		float sun[4];
		float eyeToWorld[16];
		float terrainAxes[4];
		float terrainGrid[4];
		float terrainShade[4];
		float terrainCell[4];
	};

	/**
	 * The Vulkan device, swapchain and fixed function emulation behind the driver.
	 *
	 * The driver turns the game's OpenGL-shaped calls into state; this turns that state
	 * into Vulkan work: pipelines keyed on vertex format, blend, depth and stencil state,
	 * push constants for everything that changes per draw, textures with their samplers,
	 * and the offscreen buffer regions the city view is saved into and restored from.
	 *
	 * The game draws into a back buffer of scvk's own, the size of the video mode, that
	 * survives from one frame to the next as a DirectX back buffer does: the game only
	 * redraws what changed, and the rest of the picture has to still be there. Presenting
	 * copies it into the swapchain image, scaling when the window is another size.
	 *
	 * Two frames are in flight. The CPU records one while the GPU draws the one before,
	 * and the per-frame memory is handed out from rings that wait for a frame only when
	 * they would otherwise overwrite something it still reads.
	 *
	 * The source is split by concern: VulkanBackend.cpp holds the device, swapchain,
	 * frames and captures, VulkanBackend_Draw.cpp the pipelines and draws,
	 * VulkanBackend_Textures.cpp the textures and samplers, VulkanBackend_Regions.cpp the
	 * depth buffer and buffer regions, and VulkanBackend_Effects.cpp the blits, the shadow
	 * passes and what ReShade and other plugins are handed.
	 *
	 * Every entry point is safe to call when initialisation failed. The game cannot be
	 * allowed to crash because a machine has no Vulkan driver, so a dead backend simply
	 * does nothing and says so once in the log. A lost device is rebuilt instead.
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

			// Whether back faces are culled, pipeline state as well.
			bool     isFaceCullingEnabled;

			// The stencil test and its operations, in the game's numbering. The reference
			// and masks are dynamic state.
			bool     isStencilTestEnabled;
			uint32_t stencilComparison;
			uint32_t stencilFailOperation;
			uint32_t stencilDepthFailOperation;
			uint32_t stencilPassOperation;

			// Flat shading takes the colour of a primitive's first vertex, as Direct3D does,
			// which needs shader variants whose colour is not interpolated.
			bool     isFlatShaded;

			bool operator==(PipelineKey const& other) const = default;
		};

		struct PipelineKeyHash
		{
			size_t operator()(PipelineKey const& key) const;
		};

		/** Where a format's attributes live, decoded once per format. */
		struct VertexLayout
		{
			uint32_t stride       = 0;
			bool     hasColour    = false;
			uint32_t colourOffset = 0;

			// Coordinate sets, not components. The terrain carries two, one per texture
			// stage; buildings carry one, which the second stage shares or replaces with
			// coordinates generated from the position.
			uint32_t textureCoordinateSets      = 0;
			uint32_t textureCoordinateOffset[2] = { 0, 0 };
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
		 * What every vertex of a draw reads besides its own attributes, as per-instance
		 * attributes, because the push constant block is full.
		 *
		 * The fog: the dot product of its row with the object position is the eye-space
		 * depth in front of the camera, and its parameters are the mode (0 off, 1
		 * exponential, 2 squared exponential, 3 linear), the density, and the scale and
		 * offset the linear equation reduces to.
		 *
		 * Each stage's coordinates: the two rows of its texture matrix a 2D sample reads,
		 * eight floats per stage, and where its input comes from, 0 the first set, 1 the
		 * second and 2 the object position. The vertex stage works them out, so the vertex
		 * copy is the game's own bytes, unchanged.
		 */
		struct DrawRecord
		{
			float fogDistanceRow[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
			float fogParameters[4]  = { 0.0f, 1.0f, 0.0f, 1.0f };
			float fogColour[4]      = { 0.0f, 0.0f, 0.0f, 0.0f };
			float stageRows[16]     = { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f };
			float stageSources[4]   = { 0.0f, 1.0f, 0.0f, 0.0f };
		};

		/** The push constant block of the geometry pipelines, laid out as both shader stages declare it. */
		struct PushConstantBlock
		{
			float    transform[16];
			float    fragmentState[4];
			uint32_t combinerState[4];
			float    constantColour[4];
			float    sceneTint[4];
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

			// A name handed out by ReserveTexture that no image backs yet. The game's
			// GenTextures and TexImage2D path names a texture before saying its size.
			bool            isReserved   = false;

			// The game's internal format, kept so a lost device can rebuild the texture.
			uint32_t        internalFormat = 0;

			// Distinguishes this texture from whatever reuses its handle later, so a draw
			// captured for the shadow map can tell it still names the same picture.
			uint32_t        serial = 0;

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

			// The frame whose command buffer last sampled this texture. Uploads go to the
			// GPU ahead of the frame's draws, so an upload in that same frame reaches
			// draws that the game issued before it.
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
			VkImageView     secondView          = VK_NULL_HANDLE;
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

		/** An image scvk renders into or samples, with the views it needs. */
		struct RenderImage
		{
			VkImage        image      = VK_NULL_HANDLE;
			VkDeviceMemory memory     = VK_NULL_HANDLE;
			VkImageView    view       = VK_NULL_HANDLE;
			VkImageView    secondView = VK_NULL_HANDLE;
			VkFormat       format     = VK_FORMAT_UNDEFINED;
			uint32_t       width      = 0;
			uint32_t       height     = 0;
			VkImageLayout  layout     = VK_IMAGE_LAYOUT_UNDEFINED;
		};

		/** One host-visible buffer of an arena, mapped for its whole life. */
		struct ArenaBlock
		{
			VkBuffer       buffer = VK_NULL_HANDLE;
			VkDeviceMemory memory = VK_NULL_HANDLE;
			void*          mapped = nullptr;

			// The last frame that took space in it. It cannot be rewound until the GPU has
			// finished that frame.
			uint64_t       lastFrame = 0;
		};

		/**
		 * Per-frame vertex, index, staging or uniform data, handed out from a ring of
		 * mapped blocks. A draw is recorded now and runs later, so the bytes have to stay
		 * put until the GPU has finished the frame that reads them.
		 *
		 * A frame carries on in the block the last one stopped in, and moves to the next
		 * block when that one is full, waiting first for the frame that last wrote there
		 * if the GPU has not finished it, or adding a block while there is room for one.
		 * Only a single frame that fills every block has to be submitted part way.
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

		/**
		 * What one frame of the two in flight owns: the command buffers it records, the
		 * fence its last submit signals, the semaphore its swapchain image arrives on, and
		 * what it retired, which waits for that fence.
		 */
		struct FrameSlot
		{
			VkCommandPool                commandPool    = VK_NULL_HANDLE;
			std::vector<VkCommandBuffer> commandBuffers;
			uint32_t                     usedCommandBuffers = 0;
			VkFence                      fence          = VK_NULL_HANDLE;
			VkSemaphore                  imageAvailable = VK_NULL_HANDLE;
			VkDescriptorPool             transientPool  = VK_NULL_HANDLE;
			uint32_t                     transientSets  = 0;
			std::vector<RetiredImage>    retiredImages;
			std::vector<VkBuffer>        retiredBuffers;
			std::vector<VkDeviceMemory>  retiredMemory;

			// The frame whose work the fence stands for, and whether that work is still to
			// be waited for.
			uint64_t                     serial         = 0;
			bool                         isFencePending = false;
		};

		/**
		 * Where a frame's time goes, for the heartbeat's split of slow frames.
		 *
		 * A phase entered inside another pauses the outer one, so a frame's phases add up
		 * to the gap between its present and the one before. Game is everything outside
		 * scvk's timed work, which is mostly the game's own.
		 */
		enum FramePhase : uint32_t
		{
			FRAME_PHASE_GAME,
			FRAME_PHASE_RECORDING,
			FRAME_PHASE_VERTEX_COPIES,
			FRAME_PHASE_PIPELINES,
			FRAME_PHASE_TEXTURES,
			FRAME_PHASE_SUBMITS,
			FRAME_PHASE_GPU_WAITS,
			FRAME_PHASE_SWAPCHAIN,
			FRAME_PHASE_COUNT,
		};

		/** Charges the time until it goes out of scope to a phase, then resumes the phase it interrupted. */
		class PhaseScope
		{
		public:
			PhaseScope(VulkanBackend& backend, FramePhase phase) : backend(backend), interruptedPhase(backend.EnterPhase(phase)) {}
			~PhaseScope() { backend.EnterPhase(interruptedPhase); }

			PhaseScope(PhaseScope const&) = delete;
			PhaseScope& operator=(PhaseScope const&) = delete;

		private:
			VulkanBackend& backend;
			FramePhase     interruptedPhase;
		};

		/** What the command buffer being recorded already has bound, so a draw binds only what changed. */
		struct BindingCache
		{
			VkPipeline      pipeline         = VK_NULL_HANDLE;
			VkPipelineLayout layout          = VK_NULL_HANDLE;
			VkDescriptorSet sets[4]          = {};
			VkBuffer        vertexBuffer     = VK_NULL_HANDLE;
			VkDeviceSize    vertexOffset     = 0;
			VkBuffer        recordBuffer     = VK_NULL_HANDLE;
			VkDeviceSize    recordOffset     = 0;
			VkBuffer        indexBuffer      = VK_NULL_HANDLE;
			VkViewport      viewport         = {};
			VkRect2D        scissor          = {};
			bool            hasViewport      = false;
			bool            hasPushConstants = false;
			PushConstantBlock pushConstants  = {};
			int32_t         depthBias        = INT32_MIN;
			uint32_t        stencilReference = UINT32_MAX;
			uint32_t        stencilCompareMask = UINT32_MAX;
			uint32_t        stencilWriteMask = UINT32_MAX;
			bool            hasStencilState  = false;
		};

		/** Which render pass, if any, the command buffer is inside. */
		enum class ActivePass
		{
			None,
			Main,
			ColourOnly,
		};

		//// Constants

	public:
		/** Frames the CPU may record ahead of the GPU. */
		static constexpr uint32_t FRAMES_IN_FLIGHT = 2;

	private:
		// Depth is read and written in both fragment test stages, early when the shader
		// cannot discard and late when it can, so a dependency on depth names both.
		static constexpr VkPipelineStageFlags DEPTH_STAGES = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;

		// Distinct filter and wrap combinations; the game uses a handful.
		static constexpr size_t MAXIMUM_SAMPLERS = 64;

		//// State

		// The instance and the device
		VkDebugUtilsMessengerEXT debugMessenger = VK_NULL_HANDLE;
		VkInstance               instance       = VK_NULL_HANDLE;
		VkPhysicalDevice         physicalDevice = VK_NULL_HANDLE;
		VkPhysicalDeviceProperties physicalDeviceProperties{};
		VkDevice                 device         = VK_NULL_HANDLE;
		VkQueue                  queue          = VK_NULL_HANDLE;
		uint32_t                 queueFamily    = UINT32_MAX;
		std::string              deviceName;
		std::string              apiVersion;
		bool                     isDead         = false;
		VkPipelineCache          pipelineCache  = VK_NULL_HANDLE;

		// A lost device is torn down and rebuilt at the next frame boundary, backing off
		// between attempts that fail. The generation counts the devices made, so a plugin
		// holding objects of an old one can tell.
		bool     isDeviceLost           = false;
		bool     isRecoveringDevice     = false;
		uint32_t deviceRecoveryFailures = 0;
		uint64_t nextDeviceRecoveryTick = 0;
		uint32_t deviceGeneration       = 0;
		void   (*beforeDeviceDestroyHook)(void* context) = nullptr;
		void*    beforeDeviceDestroyHookContext          = nullptr;

		// Pipelines made since the cache file was last written.
		uint32_t unsavedPipelines       = 0;

		// Whether the window is in exclusive fullscreen, which the driver says before the
		// device is made, and whether the swapchain took the flip model's extra image.
		bool isExclusiveFullscreen = false;
		bool isFlipModel           = true;

		// Whether the instance can and the device does take a fullscreen policy for the
		// swapchain. See CreateSwapchain.
		bool canAskFullscreenPolicy     = false;
		bool hasFullscreenPolicyControl = false;

		// Vulkan only promises 4096 allocations, so every one is counted against the
		// device's own limit.
		uint32_t maximumMemoryAllocations = 0;
		uint32_t liveMemoryAllocations    = 0;

		// The memory type the arenas take, and whether it is on the device. A
		// device-local type the CPU can write, which resizable BAR offers, saves the GPU
		// reading every vertex across the bus.
		uint32_t arenaMemoryType          = UINT32_MAX;
		bool     isArenaMemoryDeviceLocal = false;

		// The window, the swapchain and the present
		void*                      windowHandle    = nullptr;
		VkSurfaceKHR               surface         = VK_NULL_HANDLE;
		VkSwapchainKHR             swapchain       = VK_NULL_HANDLE;
		VkFormat                   swapchainFormat = VK_FORMAT_UNDEFINED;
		VkExtent2D                 swapchainExtent{};
		VkPresentModeKHR           presentMode     = VK_PRESENT_MODE_FIFO_KHR;
		std::vector<VkImage>       swapchainImages;
		std::vector<VkSemaphore>   renderFinishedSemaphores;
		bool                       isPresentationPaused = false;
		VkResult                   lastPresentResult    = VK_SUCCESS;

		// What the game draws into. The back buffer and depth buffer are the size of the
		// video mode and outlive the swapchain, which is the size of the window.
		uint32_t      renderWidth  = 0;
		uint32_t      renderHeight = 0;
		RenderImage   backBuffer;
		RenderImage   depthBuffer;
		bool          hasStencil          = false;
		bool          isDepthSampleable   = false;
		VkRenderPass  renderPass          = VK_NULL_HANDLE;
		VkRenderPass  colourOnlyPass      = VK_NULL_HANDLE;
		VkFramebuffer framebuffer         = VK_NULL_HANDLE;
		VkFramebuffer colourOnlyFramebuffer = VK_NULL_HANDLE;
		ActivePass    activePass          = ActivePass::None;

		// Set when something outside scvk, ReShade or a frame callback, last touched the
		// back buffer, so the next use waits for it.
		bool          isExternalWorkPending = false;

		// Objects retired between frames. The frame before may still be on the GPU, so
		// they join the next frame's slot once it has been flushed, rather than the slot
		// about to be flushed.
		std::vector<RetiredImage> pendingRetiredImages;

		// The frames. The serial counts frames begun; the completed one is the newest the
		// GPU is known to have finished.
		FrameSlot       frameSlots[FRAMES_IN_FLIGHT];
		uint32_t        currentSlot       = 0;
		uint64_t        frameSerial       = 1;
		uint64_t        completedSerial   = 0;
		VkCommandBuffer commandBuffer     = VK_NULL_HANDLE;
		bool            isFrameActive     = false;
		uint64_t        presentedFrames   = 0;

		// What the command buffer being recorded has bound.
		BindingCache    bound;

		// The performance counter at the last heartbeat, for the frame rate it reports.
		int64_t lastHeartbeatTicks = 0;

		// The gaps between presents since the last heartbeat. A stall shows up here even
		// when the average rate hides it, which is what a texture reload looks like.
		int64_t  ticksPerSecond    = 0;
		int64_t  lastPresentTicks  = 0;
		int64_t  slowestFrameTicks = 0;
		uint32_t slowFrames        = 0;

		// The phase clock. The running frame's time is split between the phases as it
		// passes, then added at its present to the totals over every frame and over the
		// slow ones, and kept when it is the slowest, until the next heartbeat.
		//
		// Only at the debug level, which is the only one its report reaches: a draw passes
		// through several phases, and reading the clock for each, over a hundred thousand
		// draws in a city redraw at the widest zoom, is time the player would lose.
		bool       isPhaseTimingEnabled                      = false;
		FramePhase activePhase                               = FRAME_PHASE_GAME;
		int64_t    phaseStartTicks                           = 0;
		int64_t    framePhaseTicks[FRAME_PHASE_COUNT]        = {};
		int64_t    allFramesPhaseTicks[FRAME_PHASE_COUNT]    = {};
		int64_t    slowFramesPhaseTicks[FRAME_PHASE_COUNT]   = {};
		int64_t    slowestFramePhaseTicks[FRAME_PHASE_COUNT] = {};

		// Per-frame geometry, staging and uniforms, and the static indices turning
		// consecutive quads into triangle pairs.
		Arena          vertexArena;
		Arena          indexArena;
		Arena          stagingArena;
		Arena          uniformArena;
		VkBuffer       quadIndexBuffer = VK_NULL_HANDLE;
		VkDeviceMemory quadIndexMemory = VK_NULL_HANDLE;
		uint32_t       quadCapacity    = 0;

		// Shaders and pipelines. The vertex modules are indexed by flat * 6 + hasColour * 3
		// + textureCoordinateSets, the fragment modules by flat.
		VkShaderModule             vertexModules[13]  = {};
		VkShaderModule             fragmentModules[3] = {};

		// Whether the device takes the lit variant's 29 vertex attributes and its outputs
		bool isPerPixelLightingSupported = false;
		VkPipelineLayout           pipelineLayout     = VK_NULL_HANDLE;
		std::unordered_map<PipelineKey, VkPipeline, PipelineKeyHash> pipelines;
		PipelineKey                lastPipelineKey{};
		VkPipeline                 lastPipeline       = VK_NULL_HANDLE;

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
		bool     isFaceCullingEnabled   = false;
		bool     isFlatShaded           = false;

		// The stencil test, which the depth buffer carries when its format has room, and
		// the polygon offset, a depth bias in the depth buffer's own units.
		bool     isStencilTestEnabled      = false;
		uint32_t stencilComparison         = 7;
		uint32_t stencilReference          = 0;
		uint32_t stencilReadMask           = 0xff;
		uint32_t stencilWriteMask          = 0xff;
		uint32_t stencilFailOperation      = 0;
		uint32_t stencilDepthFailOperation = 0;
		uint32_t stencilPassOperation      = 0;
		int32_t  polygonOffset             = 0;

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

		// Where each stage's texture coordinates come from, which decides whether the second
		// stage can run. The vertex stage reads them from the draw record.
		StageCoordinates stageCoordinates[2] = { StageCoordinates{}, StageCoordinates{ false, 1, { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f } } };

		// The fog and the stages' coordinates, which reach the vertex stage through a second
		// vertex binding because the push constant block is full. The record is written
		// into the vertex arena when it changes and every draw until the next change binds
		// the same copy; a null buffer means there is no current copy.
		DrawRecord   drawRecord;
		bool         isFogEnabled     = false;
		VkBuffer     drawRecordBuffer = VK_NULL_HANDLE;
		VkDeviceSize drawRecordOffset = 0;

		// Diagnostics that replace every colour on screen.
		bool shouldShowPassColours = false;
		int  debugChannel          = -1;

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
		std::vector<SamplerEntry>   samplers;
		uint32_t                    currentTexture  = 0;
		uint32_t                    currentTexture1 = 0;
		uint32_t                    nextTextureSerial = 1;

		// Each stage's magnification filter, minification filter, wrap S and wrap T, in the
		// game's own numbering. They belong to the stage, so binding another texture there
		// leaves them alone. Linear with repeat until the game says otherwise.
		uint32_t                    stageParameters[2][4] = { { 1, 1, 3, 3 }, { 1, 1, 3, 3 } };

		// Commands run outside the frame and waited for at once, which reading the back
		// buffer between frames needs.
		VkCommandPool               utilityCommandPool  = VK_NULL_HANDLE;
		VkCommandBuffer             uploadCommandBuffer = VK_NULL_HANDLE;
		VkFence                     uploadFence         = VK_NULL_HANDLE;

		// Texture uploads, recorded into one batch and staged in an arena, then submitted
		// ahead of the frame that draws with them. Waiting for each upload on its own cost
		// most of a millisecond, and a modded city makes thousands of them.
		Arena                       textureUploadArena;
		std::vector<ArenaBlock>     oversizedUploadBuffers;
		VkCommandBuffer             textureBatchCommandBuffer = VK_NULL_HANDLE;
		VkFence                     textureBatchFence         = VK_NULL_HANDLE;
		bool                        isTextureBatchOpen        = false;
		bool                        isTextureBatchInFlight    = false;

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
		uint32_t     textureBatches     = 0;
		int64_t      textureWorkTicks   = 0;
		int64_t      textureMemoryTicks = 0;

		// Vertex traffic since the last heartbeat, and the most any one frame copied, which
		// is what the vertex arena has to hold.
		uint32_t     vertexUploads           = 0;
		VkDeviceSize vertexUploadBytes       = 0;
		VkDeviceSize frameVertexBytes        = 0;
		VkDeviceSize largestFrameVertexBytes = 0;
		uint32_t     arenaWaits              = 0;
		uint32_t     partialSubmits          = 0;

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

		// The blits: an image that only grows, holding the latest one's pixels, with the
		// pipelines that draw it as a quad, opaque or blended.
		RenderImage     blitImage;
		VkDescriptorSet blitImageSet         = VK_NULL_HANDLE;
		uint32_t        blitImagePoolIndex   = 0;
		VkShaderModule  blitVertexModule     = VK_NULL_HANDLE;
		VkShaderModule  blitFragmentModule   = VK_NULL_HANDLE;
		VkPipelineLayout blitPipelineLayout  = VK_NULL_HANDLE;
		VkPipeline      blitPipelines[2]     = {};

		// Fixed samplers for the passes of scvk's own: point and linear, clamped and
		// repeating, each with the set that binds it.
		VkSampler       pointClampSampler    = VK_NULL_HANDLE;
		VkSampler       linearClampSampler   = VK_NULL_HANDLE;
		VkSampler       linearWrapSampler    = VK_NULL_HANDLE;
		VkDescriptorSet pointClampSet        = VK_NULL_HANDLE;
		VkDescriptorSet linearClampSet       = VK_NULL_HANDLE;
		VkDescriptorSet linearWrapSet        = VK_NULL_HANDLE;

		// The shadow map and what draws into it and composites it.
		RenderImage           shadowMap;
		VkRenderPass          shadowPass                 = VK_NULL_HANDLE;
		VkFramebuffer         shadowFramebuffer          = VK_NULL_HANDLE;
		VkShaderModule        shadowVertexModule         = VK_NULL_HANDLE;
		VkShaderModule        shadowFragmentModule       = VK_NULL_HANDLE;
		VkShaderModule        fullscreenVertexModule     = VK_NULL_HANDLE;
		VkShaderModule        compositeFragmentModule    = VK_NULL_HANDLE;
		VkPipelineLayout      shadowPipelineLayout       = VK_NULL_HANDLE;
		VkPipeline            shadowCasterPipeline       = VK_NULL_HANDLE;
		VkPipeline            shadowLinePipeline         = VK_NULL_HANDLE;
		VkPipeline            shadowPointPipeline        = VK_NULL_HANDLE;
		VkPipeline            shadowRegistryPipeline     = VK_NULL_HANDLE;
		VkDescriptorSetLayout compositeSetLayout         = VK_NULL_HANDLE;
		VkPipelineLayout      compositePipelineLayout    = VK_NULL_HANDLE;
		VkPipeline            compositePipeline          = VK_NULL_HANDLE;
		RenderImage           terrainCeilingImage;
		RenderImage           terrainVertexImage;
		bool                  isShadowMapPassActive      = false;
		bool                  hasShadowResources         = false;

		// The scene depth re-encoded for ReShade's DEPTH semantic.
		RenderImage           sceneDepthImage;
		VkRenderPass          sceneDepthPass             = VK_NULL_HANDLE;
		VkFramebuffer         sceneDepthFramebuffer      = VK_NULL_HANDLE;
		VkShaderModule        sceneDepthFragmentModule   = VK_NULL_HANDLE;
		VkDescriptorSetLayout sceneDepthSetLayout        = VK_NULL_HANDLE;
		VkPipelineLayout      sceneDepthPipelineLayout   = VK_NULL_HANDLE;
		VkPipeline            sceneDepthPipeline         = VK_NULL_HANDLE;

		//// Private Functions

		// Device and swapchain, in VulkanBackend.cpp

		/**
		 * Logs once per distinct failure, then goes quiet. A lost device is noted for the
		 * next frame boundary to rebuild; anything else leaves the backend dead.
		 */
		void Fail(char const* what, VkResult result);

		/** Notes a result that may mean the device is gone. Returns whether it does. */
		bool NoteDeviceLoss(VkResult result);

		bool PickPhysicalDevice(void);
		bool CreateLogicalDevice(void);
		bool CreateFrameResources(void);
		void DestroyFrameResources(void);
		bool CreateSwapchain(void);
		bool CreateRenderTargets(void);
		void DestroyRenderTargets(void);
		bool CreateRenderPasses(void);
		void DestroySwapchain(void);
		void LoadPipelineCache(void);
		void SavePipelineCache(void);
		void ChoosePresentMode(void);

		/** Makes the swapchain again once the window has area, or returns false while it has none. */
		bool RestoreSwapchain(void);

		/** Destroys everything that belongs to the device and the window, keeping the instance. */
		void DestroyDevice(void);

		/** Destroys the device and makes it again, recreating every texture and region empty. */
		bool RecoverDevice(void);

		bool FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags properties, uint32_t& outIndex) const;

		/** vkAllocateMemory, counting the allocation against the device's limit. */
		VkResult AllocateDeviceMemory(VkMemoryAllocateInfo const& information, VkDeviceMemory& outMemory);

		/** vkFreeMemory for memory from AllocateDeviceMemory, clearing the handle. Does nothing for a null one. */
		void FreeDeviceMemory(VkDeviceMemory& memory);

		bool CreateHostBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& outBuffer, VkDeviceMemory& outMemory, void*& outMapped);

		/** Creates an image with memory of its own and the views asked for. */
		bool CreateRenderImage(VkFormat format, uint32_t width, uint32_t height, VkImageUsageFlags usage, VkImageAspectFlags viewAspect, VkFormat secondViewFormat, VkImageCreateFlags flags, RenderImage& outImage);
		void DestroyRenderImage(RenderImage& image);

		/** Retires a render image to the frame being recorded, so it outlives the GPU's use of it. */
		void RetireRenderImage(RenderImage& image);

		/** Hands objects to the frame being recorded, or the next one between frames, to destroy once the GPU is done. */
		void Retire(RetiredImage const& retired);

		/** The render size as signed numbers, for arithmetic against the game's signed rectangles. */
		int32_t RenderWidth(void) const;
		int32_t RenderHeight(void) const;

		/** Starts a frame if one is not already in progress. */
		bool EnsureFrame(void);

		/** Begins the next command buffer of the current frame. */
		bool BeginCommandBuffer(void);

		/** Ends and submits the command buffer being recorded, optionally signalling the frame's fence. */
		bool SubmitCommandBuffer(VkSemaphore waitSemaphore, VkPipelineStageFlags waitStages, VkSemaphore signalSemaphore, bool shouldSignalFence);

		/** Submits what the frame has recorded so far, waits for it, and carries on recording the same frame. */
		bool SubmitFrameSoFar(void);

		/** Waits for the GPU to finish a frame, if it has not already. */
		bool WaitForFrame(uint64_t serial);

		/** vkQueueSubmit of one batch, timed as a submit. */
		VkResult SubmitToQueue(VkSubmitInfo const& submit, VkFence fence);

		/** vkWaitForFences on one fence, timed as waiting for the GPU. */
		VkResult WaitForFence(VkFence fence, uint64_t timeoutNanoseconds);

		/** Charges the time since the last switch to the active phase and makes another active. Returns the one it replaced. */
		FramePhase EnterPhase(FramePhase phase);

		/** Counts the frame ending at this present: its gap since the last one and where that time went. */
		void TimeFrame(int64_t nowTicks);

		/** One heartbeat line splitting a time by phase. */
		void LogPhaseTicks(char const* heading, int64_t const phaseTicks[FRAME_PHASE_COUNT]) const;

		/** Barriers an image of scvk's own into a layout, tracking where it was. */
		void TransitionImage(RenderImage& image, VkImageLayout newLayout, VkImageAspectFlags aspect);

		/** Barriers the back buffer into a layout. */
		void TransitionTo(VkImageLayout newLayout);

		/** Barriers the depth buffer into a layout. */
		void TransitionDepth(VkImageLayout newLayout);

		/** What a layout implies about access and pipeline stage. */
		static void LayoutAccess(VkImageLayout layout, VkAccessFlags& outAccess, VkPipelineStageFlags& outStages);

		/** The aspects of the depth buffer: depth, and stencil when it has one. */
		VkImageAspectFlags DepthAspects(void) const;

		/** Orders the next use of the back buffer after work recorded outside scvk. */
		void WaitForExternalWork(void);

		void BeginRenderPassIfNeeded(void);
		void BeginColourOnlyPass(void);
		void EndRenderPassIfActive(void);
		void ApplyViewport(void);

		/** Forgets what the command buffer has bound, after something else bound its own. */
		void InvalidateBindings(void);

		/**
		 * The viewport in framebuffer coordinates, clamped to the back buffer. Returns
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

		/** Records the copy of the back buffer for a requested frame capture. */
		bool RecordFrameCapture(void);

		/** Writes whichever capture this frame recorded, once the GPU has finished it. */
		void WriteCapture(bool isRegion, uint32_t regionWidth, uint32_t regionHeight);

		/** Records the copy of the back buffer into the swapchain image, scaling it when the two differ. */
		void RecordPresentCopy(uint32_t swapchainImageIndex);

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
		VkPipeline CreatePipeline(PipelineKey const& key);
		void DestroyPipelines(void);

		/** Where a format's attributes live, from the game's packed encoding. */
		static VertexLayout DecodeVertexLayout(uint32_t gdVertexFormat);

		/** The game's primitive numbering to a Vulkan topology. */
		static bool MapTopology(uint32_t gdPrimitiveType, VkPrimitiveTopology& outTopology, bool& outIsQuadList);

		/** Adds one block to an arena. */
		bool ArenaAddBlock(Arena& arena);

		/**
		 * Reserves space, moving on through the ring when this block is full: to a block
		 * the GPU has finished with, to a new one while the arena may grow, or to one the
		 * GPU still reads after waiting for it. Fails only when a single frame has filled
		 * every block.
		 */
		bool ArenaAllocate(Arena& arena, VkDeviceSize bytes, VkDeviceSize alignment, VkBuffer& outBuffer, VkDeviceSize& outOffset, uint8_t*& outAddress);

		/** Whether an arena can hand out this much, in one piece, without the frame being submitted part way. */
		bool ArenaHasRoom(Arena const& arena, VkDeviceSize bytes, VkDeviceSize alignment) const;

		/** Rewinds an arena to its first block, once nothing on the GPU reads it. */
		static void ArenaRewind(Arena& arena);

		void DestroyArena(Arena& arena);

		/**
		 * Makes room for one draw's vertices and indices, submitting the frame so far when
		 * the arenas are full. The stride, when given, is the alignment the vertices take.
		 */
		bool ReserveDrawSpace(VkDeviceSize vertexBytes, VkDeviceSize indexBytes, uint32_t vertexStride = 0);

		/**
		 * Copies a vertex range into the per-frame arena, at a whole number of vertices
		 * from the start of its block, and says which vertex of the block it starts at.
		 */
		bool UploadVertices(void const* vertices, uint32_t firstVertex, uint32_t vertexCount, VertexLayout const& layout, VkBuffer& outBuffer, uint32_t& outBaseVertex);

		/** Whether the second stage takes part in the draw. */
		bool IsTwoStageDraw(uint32_t textureCoordinateSets) const;

		/** Whether the draw runs the combiner network, on both stages or on the first alone. */
		bool IsCombinerDraw(uint32_t textureCoordinateSets) const;

		/** The heartbeat's line on vertex traffic since the last one. */
		void LogVertexTraffic(void);

		/** Everything a draw needs bound, shared by the indexed and plain paths. */
		bool BindDrawState(uint32_t gdVertexFormat, VkPrimitiveTopology topology, VkBuffer vertexBuffer, VkDeviceSize vertexOffset, VertexLayout const& layout);

		/** Binds the geometry pipeline layout's descriptor sets that changed. */
		void BindDescriptorSets(VkPipelineLayout layout, VkDescriptorSet const sets[4]);

		/** Sets the depth bias and the stencil state the dynamic state carries. */
		void ApplyDynamicState(void);

		/** Pushes the per-draw constants, adjusted for a disabled first stage and the diagnostics. */
		void PushDrawConstants(bool isTwoStage, bool isCombining);

		/** Binds the textures and sampler of both stages, applying their parameters. */
		void BindTextures(bool isTwoStage);

		/** Binds an index buffer at offset 0 unless it is bound already. Indices are then addressed through firstIndex. */
		void BindIndexBuffer(VkBuffer buffer);

		/** Changes the draw record, so the next draw writes a fresh copy of it. */
		void UpdateDrawRecord(DrawRecord const& record);

		/** The current draw record's copy in the vertex arena, written first if there is none. */
		bool GetDrawRecordCopy(VkBuffer& outBuffer, VkDeviceSize& outOffset);

		// Textures, in VulkanBackend_Textures.cpp

		bool CreateDescriptorResources(void);
		bool CreateDefaultTexture(void);
		bool CreateFixedSamplers(void);
		void DestroyTextures(void);

		/** Creates the image, view and set of a texture slot from its format, size and levels. */
		bool CreateTextureObjects(Texture& texture);

		/** Retires the image, view and set of a texture slot, leaving its description. */
		void RetireTextureObjects(Texture& texture);

		/** Finds memory for a texture image, in a texture block unless it is larger than one. */
		bool AllocateTextureMemory(VkMemoryRequirements const& requirements, ImageMemory& outMemory);

		/** Takes the first free range of a block that fits, aligned. Returns false when none does. */
		static bool TakeBlockRange(TextureBlock& block, VkDeviceSize size, VkDeviceSize alignment, MemoryRange& outRange, VkDeviceSize& outOffset);

		/** Gives a range back to its block, merging it with the free ranges either side. */
		static void ReturnBlockRange(TextureBlock& block, MemoryRange range);

		/** Gives an image's memory back, to its block or to the device, and clears it. */
		void ReleaseImageMemory(ImageMemory& memory);

		/** Destroys everything a frame retired, once the GPU has finished it. */
		void FlushRetired(FrameSlot& slot);

		/** The sampler for one set of filter and wrap parameters. */
		VkDescriptorSet GetSamplerSet(uint32_t const parameters[4]);

		/** A descriptor set for a new texture, from the first texture pool with room. */
		bool AllocateTextureSet(VkDescriptorSet& outSet, uint32_t& outPoolIndex);

		/** A descriptor set for this frame only, from its transient pool. */
		bool AllocateTransientSet(VkDescriptorSetLayout layout, VkDescriptorSet& outSet);

		/** Records a draw sampling a texture, and reports the hazards counted above. */
		void NoteTextureUse(uint32_t handle);

		/** Converts one level's texels into the image's own layout. Returns false for a format it cannot read. */
		bool StageTexels(Texture const& texture, uint32_t width, uint32_t height, uint32_t gdFormat, uint32_t gdType, uint32_t rowLength, void const* pixels, uint8_t* outStaged);

		/** How many bytes StageTexels writes for one level. */
		static VkDeviceSize StagedBytes(Texture const& texture, uint32_t width, uint32_t height);

		/** The heartbeat's line on live textures and the traffic since the last one. */
		void LogTextureTraffic(void);

		/** Writes an uploaded interface texture to a file, a handful of times per session. */
		void DumpUploadedTexture(Texture const& texture, uint32_t handle, uint32_t width, uint32_t height, uint32_t gdFormat, uint32_t gdType, uint32_t rowLength, void const* pixels);

		/** Starts recording into the upload command buffer. */
		void BeginUploadCommands(void);

		/** Submits the upload command buffer and waits for it. */
		void SubmitUploadCommands(void);

		/** Opens a texture batch to record into, unless one is open already. */
		bool BeginTextureBatch(void);

		/** Submits the open texture batch, without waiting for it. */
		void SubmitTextureBatch(void);

		/** Submits the open texture batch and waits for it, freeing its staging. */
		void FinishTextureBatch(void);

		/** Waits for the submitted texture batch, if there is one, and frees its staging. */
		void WaitForTextureBatch(void);

		/** Finds staging for an upload in the batch, finishing the batch first when the arena is full. */
		bool AllocateTextureStaging(VkDeviceSize bytes, VkBuffer& outBuffer, VkDeviceSize& outOffset, uint8_t*& outAddress);

		// Depth and buffer regions, in VulkanBackend_Regions.cpp

		/** Picks the depth format: with stencil where the device offers one, and sampleable where it can be. */
		bool ChooseDepthFormat(void);

		/** Barriers a region image, which is not tracked the way the back buffer is. */
		void TransitionRegion(BufferRegion const& region, VkImageLayout oldLayout, VkImageLayout newLayout);

		/** Clamps a copy between the back buffer and a region to both images. Returns false when either origin is negative. */
		bool ClampRegionCopy(BufferRegion const& region, int32_t regionX, int32_t regionY, int32_t screenX, int32_t screenY, int32_t& width, int32_t& height) const;

		/** Creates a region's image and memory at the render size, without giving it a handle. */
		bool AllocateRegionImage(bool isDepth, BufferRegion& outRegion);

		// Blits, shadows and what other modules are handed, in VulkanBackend_Effects.cpp

		bool CreateEffectResources(void);
		void DestroyEffectResources(void);
		bool CreateBlitPipelines(void);
		bool CreateShadowResources(void);
		bool CreateSceneDepthResources(void);

		/** A simple pipeline with no vertex input, for the full-screen and blit passes. */
		VkPipeline CreateFullscreenPipeline(VkShaderModule vertexModule, VkShaderModule fragmentModule, VkPipelineLayout layout, VkRenderPass pass, VkPrimitiveTopology topology, bool isAlphaBlended, bool isShadowBlend);

		/**
		 * Staging for this frame: from the staging arena, submitting the frame so far when
		 * it is full, or a buffer of its own retired with the frame when it is larger than
		 * a block.
		 */
		bool AllocateFrameStaging(VkDeviceSize bytes, VkBuffer& outBuffer, VkDeviceSize& outOffset, uint8_t*& outAddress);

		/** Uploads one of the terrain shadow textures, recreating it when its size changed. */
		bool UploadFloatTexture(RenderImage& image, VkFormat format, uint32_t width, uint32_t height, void const* texels, VkDeviceSize bytes);

	public:
		//// Public API

		VulkanBackend(void);
		~VulkanBackend(void);

		VulkanBackend(VulkanBackend const&) = delete;
		VulkanBackend& operator=(VulkanBackend const&) = delete;

		// Setup and teardown

		/** Loads Vulkan, creates the instance, and picks a physical device. Does nothing once done. */
		bool CreateInstance(void);

		/** Says whether the next window is in exclusive fullscreen, which keeps the legacy swapchain. */
		void SetExclusiveFullscreen(bool isExclusive) { isExclusiveFullscreen = isExclusive; }

		/**
		 * Creates the surface, device, swapchain and render targets for a window, drawing
		 * at the given size whatever the window's. Calling it again for a new window first
		 * destroys what the previous call made.
		 */
		bool CreateSurfaceAndDevice(void* newWindowHandle, uint32_t width, uint32_t height);

		void Destroy(void);

		/** Whether frames can be drawn: the device and render targets exist, whatever the window is doing. */
		bool IsReady(void);

		/** Whether the device was lost and is waiting to be rebuilt. */
		bool IsDeviceLost(void) const { return isDeviceLost; }

		/**
		 * Rebuilds a lost device at a frame boundary, backing off from half a second to
		 * eight between attempts that fail. Returns whether the device works now.
		 */
		bool TryRecoverDevice(void);

		/** Counts the devices made, so a plugin can tell its objects belong to an old one. */
		uint32_t DeviceGeneration(void) const { return deviceGeneration; }

		/** Called just before a device is destroyed, while its objects can still be released. */
		void SetBeforeDeviceDestroyHook(void (*hook)(void* context), void* context)
		{
			beforeDeviceDestroyHook        = hook;
			beforeDeviceDestroyHookContext = context;
		}

		/** Description of the selected GPU, for GetDriverInfo. */
		std::string const& DeviceName(void) const { return deviceName; }
		std::string const& ApiVersion(void) const { return apiVersion; }

		// Frames

		/** Clears the colour, within the scissor when a sub-viewport is in force. */
		void Clear(float red, float green, float blue, float alpha);

		/**
		 * Copies tightly packed BGRA8 pixels into the back buffer.
		 *
		 * The game hands over BGRA8 and the back buffer is B8G8R8A8_UNORM, so the pixels
		 * go straight in with no conversion. sourceWidth is the row stride of the source
		 * in pixels, which matters when the destination is clipped: the rows still have to
		 * be strided by the full source width.
		 */
		void BlitPixels(int32_t destinationX, int32_t destinationY, uint32_t width, uint32_t height, uint32_t sourceWidth, void const* pixels);

		/**
		 * Draws tightly packed BGRA8 pixels as a quad over a rectangle of the back buffer,
		 * scaled to it, the way the DirectX driver draws its blits. The colour is
		 * multiplied by the modulate colour; the source alpha is used when asked for, and
		 * texels matching the colour key, when it has one, become transparent. Sampling
		 * is point, so a 1:1 blit stays exact.
		 */
		void DrawPixels(int32_t destinationX, int32_t destinationY, int32_t destinationWidth, int32_t destinationHeight, uint32_t sourceWidth, uint32_t sourceHeight, uint8_t const* bgraPixels, float const modulate[4], float const colourKey[4], bool isSourceAlphaUsed, bool isBlended);

		/**
		 * Copies a rectangle of the screen into tightly packed BGRA8 rows, top row first,
		 * with every alpha opaque.
		 *
		 * Mid-frame that is what the frame has drawn so far, and the commands recorded so
		 * far are submitted early to wait for them. Between frames it is the frame last
		 * presented, which the back buffer still holds. The rectangle is in window
		 * coordinates measured from the top, and has to lie inside the window.
		 */
		bool ReadFramePixels(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint8_t* outPixels);

		/** The render size, which bounds what ReadFramePixels can read. */
		uint32_t FrameWidth(void) const { return renderWidth; }
		uint32_t FrameHeight(void) const { return renderHeight; }

		/** Ends the frame, submits, and presents. */
		void Present(void);

		/**
		 * Writes the next presented frame to a file, as a 32 bit BMP.
		 *
		 * Captured from the back buffer rather than from the screen. That is the only way
		 * to be sure the picture is what scvk drew: grabbing the desktop needs the window
		 * focused, cannot see a Vulkan surface reliably, and captures whatever else
		 * happens to be on screen.
		 *
		 * BMP because its 32 bit layout is already BGRA, matching the back buffer exactly,
		 * so no encoder and no conversion are needed.
		 */
		void RequestCapture(char const* path);

		/**
		 * Writes the saved colour buffer region to a file after the next present. That is
		 * the copy of the scene the game restores every frame, so it answers whether
		 * something wrong was saved into it or only drawn over it afterwards.
		 *
		 * For depth it reads the saved depth region instead, written as a width and
		 * height followed by raw 32-bit values.
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

		/**
		 * Sets the stencil test, in the game's numbering for the comparison and the
		 * operations. Ignored by a depth buffer without stencil.
		 */
		void SetStencilState(bool isTestEnabled, uint32_t comparison, uint32_t reference, uint32_t readMask, uint32_t writeMask, uint32_t failOperation, uint32_t depthFailOperation, uint32_t passOperation);

		/** The polygon offset, a constant depth bias in units of the depth buffer's resolution. */
		void SetPolygonOffset(int32_t offset);

		/** Whether primitives take the colour of their first vertex instead of interpolating it. */
		void SetFlatShading(bool isEnabled);

		/** Whether back faces are culled. Pipeline state in Vulkan. */
		void SetFaceCulling(bool isEnabled);

		/** Clears the depth attachment, and the stencil with it when asked, within the scissor of a sub-viewport. */
		void ClearDepth(bool shouldClearDepth, float depth, bool shouldClearStencil, uint32_t stencil);

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

		/** Hands out a texture name with no image behind it yet, for the GenTextures path. */
		uint32_t ReserveTexture(void);

		/**
		 * Gives a named texture an image of this format and size, as TexImage2D does for
		 * its top level. An image already matching is kept, contents and all.
		 */
		bool DefineTexture(uint32_t handle, uint32_t gdInternalFormat, uint32_t width, uint32_t height, uint32_t levels);

		/** Whether a handle names a texture, with an image behind it or reserved for one. */
		bool IsTextureName(uint32_t handle) const;

		/** Whether draws under LIT_VERTEX_FORMAT can be made on this device. */
		bool IsPerPixelLightingSupported(void) const { return isPerPixelLightingSupported; }

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

		/** The serial of the texture a handle names now, or 0 when it names none. */
		uint32_t TextureSerial(uint32_t handle) const;

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

		/** Copies a rectangle of the back buffer into a region. */
		bool SaveBufferRegion(uint32_t handle, int32_t regionX, int32_t regionY, int32_t width, int32_t height, int32_t screenX, int32_t screenY);

		/** Copies a rectangle of a region back into the back buffer. */
		bool RestoreBufferRegion(uint32_t handle, int32_t regionX, int32_t regionY, int32_t width, int32_t height, int32_t screenX, int32_t screenY);

		bool IsBufferRegion(uint32_t handle) const;
		void DestroyBufferRegion(uint32_t handle);
		void DestroyAllBufferRegions(void);

		// Shadows

		/** Whether the shadow passes can run: the depth buffer can be sampled and their resources exist. */
		bool CanDrawShadows(void);

		/**
		 * Draws casters into the shadow map, cleared first. The vertices and indices are
		 * copied; each draw names its range of them, its texture and its constants.
		 */
		bool DrawShadowCasters(ShadowVertex const* vertices, uint32_t vertexCount, uint32_t const* indices, uint32_t indexCount, ShadowCasterDraw const* draws, uint32_t drawCount);

		/** Uploads the terrain's shadow ceiling and its height field, one RGBA float texel per vertex. */
		bool UploadTerrainShadowMaps(float const* ceiling, uint32_t ceilingWidth, uint32_t ceilingHeight, float const* vertices, uint32_t verticesX, uint32_t verticesZ);

		/**
		 * Darkens the back buffer where the scene depth lies in shadow, within a rectangle
		 * of window pixels measured from the top left. Uses the shadow map drawn last and,
		 * when asked, the terrain maps uploaded last.
		 */
		void CompositeShadows(ShadowCompositeConstants const& constants, int32_t const rectangle[4], bool isTerrainUsed);

		// What ReShade and other plugins are handed

		/** The back buffer's views, linear and sRGB, for ReShade's render targets. */
		VkImageView BackBufferView(bool isSrgb) const;

		/**
		 * Re-encodes the scene depth into an R32 float image ReShade can sample, so that
		 * ReShade.fxh's linearization with this far plane returns the depth unchanged.
		 * Returns the image's view, or null when it could not.
		 */
		VkImageView PrepareSceneDepth(float farPlane);

		/**
		 * Submits what the frame has recorded so far without waiting, leaving the back
		 * buffer as a colour attachment and the scene depth image ready to sample, so work
		 * submitted after it by someone else sees the frame so far. The frame carries on in
		 * a new command buffer that waits for that work.
		 */
		bool SubmitForExternalWork(void);

		/**
		 * Describes the frame to a plugin, starting one if needed. The back buffer is left
		 * a colour attachment outside any render pass. Call ReturnExternalFrame after.
		 */
		bool BorrowExternalFrame(ExternalFrame& outFrame);

		/** Takes the frame back after a plugin recorded into it, forgetting what was bound. */
		void ReturnExternalFrame(void);

		/** The instance, device and queue alone, for a plugin letting go of what it made with them. */
		void DescribeDevice(ExternalFrame& outFrame) const;
	};
}
