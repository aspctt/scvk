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
#include <memory>
#include <string>
#include <vector>

#include <cIGZGDriver.h>
#include <cRZRefCount.h>
#include <sGDMode.h>

#include <ext/cIGZGBufferRegionExtension.h>
#include <ext/cIGZGDriverLightingExtension.h>
#include <ext/cIGZGDriverVertexBufferExtension.h>
#include <ext/cIGZGSnapshotExtension.h>

namespace scvk
{
	// Held by pointer so vulkan.h, and the windows.h it drags in, stay out of
	// every translation unit that only needs the driver interface.
	class VulkanBackend;

	/**
	 * scvk's implementation of SimCity 4's renderer interface.
	 *
	 * At this stage every rendering method is a logged stub. The object exists
	 * to answer three questions that have to be settled before any Vulkan work
	 * is worth writing:
	 *
	 *   1. Does the game load this DLL and select it over its built-in driver?
	 *   2. Does it survive initialisation far enough to reach a first frame?
	 *   3. Which of the ~117 interface methods does the game actually call,
	 *      in what order, and how often?
	 *
	 * The answer to (3) is the map for everything after, and it is written to
	 * scvk.log. Only the parts of the driver needed to get that far are real:
	 * video mode enumeration, window creation, and COM plumbing.
	 *
	 * The interface is shaped like OpenGL 1.2 with extensions: a fixed function
	 * pipeline, immediate-mode draws, a matrix stack, and texture environment
	 * combiners. None of that exists in Vulkan, so the eventual implementation
	 * is a fixed function emulator, with a state key driving an ubershader
	 * through a pipeline cache. That is why state is tracked here rather than
	 * forwarded.
	 */
	class cVKDriver final :
		public cIGZGDriver,
		public cIGZGBufferRegionExtension,
		public cIGZGDriverLightingExtension,
		public cIGZGDriverVertexBufferExtension,
		public cIGZGSnapshotExtension,
		public cRZRefCount
	{
	public:
		/**
		 * The game's own OpenGL driver class ID.
		 *
		 * SimCity 4 picks a renderer by class ID and has exactly three slots:
		 * DirectX, OpenGL and Software. There is no room to register a fourth,
		 * so scvk claims the OpenGL slot and outranks the built-in driver by
		 * reporting a higher version from EnumClassObjects. SCGL does the same,
		 * which is why scvk and SCGL cannot be installed at the same time.
		 */
		static constexpr uint32_t kDriverGZCLSID = 0xC4554841;

		/** Reported to the GZCOM so our registration beats the built-in driver. */
		static constexpr uint32_t kDriverVersion = 1000000;

	private:
		/** Mirrors the error codes the game's own driver reports. */
		enum class DriverError : uint32_t
		{
			OK                  = 0,
			OutOfRange          = 2,
			NotSupported        = 3,
			CreateContextFailed = 6,
			InvalidEnum         = 0x500,
			InvalidValue        = 0x501,
		};

	public:
		cVKDriver(void);
		virtual ~cVKDriver(void) override;

		static bool FactoryFunction(uint32_t riid, void** ppvObj);

	public:
		virtual bool     QueryInterface(uint32_t riid, void** ppvObj) override;
		virtual uint32_t AddRef(void) override;
		virtual uint32_t Release(void) override;

	public:
		virtual void DrawArrays(uint32_t gdPrimType, int32_t first, int32_t count) override;
		virtual void DrawElements(uint32_t gdPrimType, int32_t count, uint32_t gdType, void const* indices) override;
		virtual void InterleavedArrays(uint32_t gdVertexFormat, int32_t stride, void const* pointer) override;

		virtual uint32_t MakeVertexFormat(uint32_t count, intptr_t gdElementTypePtr) override;
		virtual uint32_t MakeVertexFormat(uint32_t gdVertexFormat) override;
		virtual uint32_t VertexFormatStride(uint32_t gdVertexFormat) override;
		virtual uint32_t VertexFormatElementOffset(uint32_t gdVertexFormat, uint32_t gdElementType, uint32_t index) override;
		virtual uint32_t VertexFormatNumElements(uint32_t gdVertexFormat, uint32_t gdElementType) override;

		virtual void Clear(uint32_t mask) override;
		virtual void ClearColor(float r, float g, float b, float a) override;
		virtual void ClearDepth(double depth) override;
		virtual void ClearStencil(int32_t s) override;

		virtual void ColorMask(bool flag) override;
		virtual void DepthFunc(uint32_t gdTestFunc) override;
		virtual void DepthMask(bool flag) override;

		virtual void StencilFunc(uint32_t gdTestFunc, int32_t ref, uint32_t mask) override;
		virtual void StencilMask(uint32_t mask) override;
		virtual void StencilOp(uint32_t gdStencilOp, uint32_t gdStencilOp2, uint32_t gdStencilOp3) override;

		virtual void BlendFunc(uint32_t gdBlendFunc, uint32_t gdBlend) override;
		virtual void AlphaFunc(uint32_t gdTestFunc, float ref) override;
		virtual void ShadeModel(uint32_t gdShade) override;

		virtual void BindTexture(uint32_t gdTextureTarget, uint32_t texture) override;
		virtual void TexImage2D(uint32_t gdTextureTarget, int32_t level, int32_t gdInternalTexFormat, int32_t width, int32_t height, int32_t border, uint32_t gdTexFormat, uint32_t gdType, void const* pixels) override;
		virtual void PixelStore(uint32_t gdParameter, int32_t param) override;

		virtual void TexEnv(uint32_t gdTextureEnvTarget, uint32_t gdTextureEnvParamType, int32_t gdTextureEnvModeParam) override;
		virtual void TexEnv(uint32_t gdTextureEnvTarget, uint32_t gdTextureEnvParamType, float const* params) override;
		virtual void TexParameter(uint32_t gdTextureTarget, uint32_t gdTextureParamType, int32_t gdTextureParam) override;

		virtual void Fog(uint32_t gdFogParamType, uint32_t gdFogParam) override;
		virtual void Fog(uint32_t gdFogParamType, float const* params) override;

		virtual void ColorMultiplier(float r, float g, float b) override;
		virtual void AlphaMultiplier(float a) override;
		virtual void EnableVertexColors(bool ambient, bool diffuse) override;

		virtual void GenTextures(int32_t count, uint32_t* textures) override;
		virtual void DeleteTextures(int32_t count, uint32_t const* textures) override;
		virtual bool IsTexture(uint32_t texture) override;
		virtual void PrioritizeTextures(int32_t count, uint32_t const* textures, float const* priorities) override;
		virtual bool AreTexturesResident(int32_t count, uint32_t const* textures, bool* residences) override;

		virtual void MatrixMode(uint32_t gdMatrixTarget) override;
		virtual void LoadMatrix(float const* m) override;
		virtual void LoadIdentity(void) override;

		virtual void Flush(void) override;
		virtual void Enable(uint32_t gdDriverState) override;
		virtual void Disable(uint32_t gdDriverState) override;
		virtual bool IsEnabled(uint32_t gdDriverState) override;

		virtual void     GetBoolean(uint32_t gdParameter, bool* params) override;
		virtual void     GetInteger(uint32_t gdParameter, int32_t* params) override;
		virtual void     GetFloat(uint32_t gdParameter, float* params) override;
		virtual uint32_t GetError(void) override;

		virtual void TexStage(uint32_t texUnit) override;
		virtual void TexStageCoord(uint32_t gdTexCoordSource) override;
		virtual void TexStageMatrix(float const* matrix, uint32_t unknown0, uint32_t unknown1, uint32_t gdTexMatFlags) override;
		virtual void TexStageCombine(eGDTextureStageCombineParamType gdParamType, eGDTextureStageCombineModeParam gdParam) override;
		virtual void TexStageCombine(eGDTextureStageCombineSourceParamType gdParamType, eGDTextureStageCombineSourceParam gdParam) override;
		virtual void TexStageCombine(eGDTextureStageCombineOperandType gdParamType, eGDBlend gdBlend) override;
		virtual void TexStageCombine(eGDTextureStageCombineScaleParamType gdParamType, eGDTextureStageCombineScaleParam gdParam) override;

		virtual void     SetTexture(uint32_t texture, uint32_t texUnit) override;
		virtual intptr_t GetTexture(uint32_t texUnit) override;
		virtual intptr_t CreateTexture(uint32_t gdInternalTexFormat, uint32_t width, uint32_t height, uint32_t levels, uint32_t gdTexHintFlags) override;
		virtual void     LoadTextureLevel(uint32_t texture, int32_t level, int32_t xoffset, int32_t yoffset, int32_t width, int32_t height, uint32_t gdTexFormat, uint32_t gdType, uint32_t rowLength, void const* pixels) override;
		virtual void     SetCombiner(cGDCombiner const& combiner, uint32_t texUnit) override;

		virtual uint32_t CountVideoModes(void) const override;
		virtual void     GetVideoModeInfo(uint32_t dwIndex, sGDMode& gdMode) override;
		virtual void     GetVideoModeInfo(sGDMode& gdMode) override;
		// The two trailing flags are of unknown meaning and are ignored. See
		// the definition: one of them looks like a visibility hint, but acting
		// on it hides the game's window.
		virtual void     SetVideoMode(int32_t newModeIndex, void* hwndProc, bool unknownFlag1, bool unknownFlag2) override;

		virtual void PolygonOffset(int32_t offset) override;

		virtual void BitBlt(int32_t destLeft, int32_t destTop, int32_t width, int32_t height, uint32_t gdTexFormat, uint32_t gdType, void const* buffer, bool unknown, void const* buffer2) override;
		virtual void StretchBlt(int32_t destLeft, int32_t destTop, int32_t destWidth, int32_t destHeight, int32_t srcWidth, int32_t srcHeight, uint32_t gdTexFormat, uint32_t gdType, void const* buffer, bool unknown, void const* buffer2) override;
		virtual void BitBltAlpha(int32_t destLeft, int32_t destTop, int32_t width, int32_t height, uint32_t gdTexFormat, uint32_t gdType, void const* buffer, bool unknown, void const* buffer2, uint32_t alpha) override;
		virtual void StretchBltAlpha(int32_t destLeft, int32_t destTop, int32_t destWidth, int32_t destHeight, int32_t srcWidth, int32_t srcHeight, uint32_t gdTexFormat, uint32_t gdType, void const* buffer, bool unknown, void const* buffer2, uint32_t alpha) override;
		virtual void BitBltAlphaModulate(int32_t destLeft, int32_t destTop, int32_t width, uint32_t gdTexFormat, uint32_t gdType, void const* buffer, bool unknown, void const* buffer2, uint32_t alpha) override;
		virtual void StretchBltAlphaModulate(int32_t destLeft, int32_t destTop, int32_t destWidth, int32_t destHeight, int32_t srcWidth, int32_t srcHeight, uint32_t gdTexFormat, uint32_t gdType, void const* buffer, bool unknown, void const* buffer2, uint32_t alpha) override;

		virtual void SetViewport(void) override;
		virtual void SetViewport(int32_t x, int32_t y, int32_t width, int32_t height) override;
		virtual void GetViewport(int32_t dimensions[4]) override;

		virtual char const* GetDriverInfo(void) const override;
		virtual uint32_t    GetGZCLSID(void) const override;

		// Init and Shutdown carry the same signature on cIGZGDriver and on
		// cIGZGBufferRegionExtension, so one definition satisfies both slots.
		virtual bool Init(void) override;
		virtual bool Shutdown(void) override;
		virtual bool IsDeviceReady(void) override;
		virtual bool Punt(uint32_t unknown, void* unknown2) override;

	public:
		virtual bool     BufferRegionEnabled(void) override;
		virtual uint32_t NewBufferRegion(int32_t gdBufferRegionType) override;
		virtual bool     DeleteBufferRegion(int32_t bufferRegion) override;
		virtual bool     ReadBufferRegion(uint32_t region, int32_t x, int32_t y, int32_t width, int32_t height, int32_t destX, int32_t destY) override;
		virtual bool     DrawBufferRegion(uint32_t region, int32_t x, int32_t y, int32_t width, int32_t height, int32_t destX, int32_t destY) override;
		virtual bool     IsBufferRegion(uint32_t bufferRegion) override;
		virtual bool     CanDoPartialRegionWrites(void) override;
		virtual bool     CanDoOffsetReads(void) override;
		virtual bool     FinalRelease(void) override;
		virtual bool     DeleteAllBufferRegions(void) override;

	public:
		virtual cIGZBuffer* CopyColorBuffer(int32_t x, int32_t y, int32_t width, int32_t height, cIGZBuffer* buffer) override;

	public:
		virtual void EnableLighting(bool enabled) override;
		virtual void EnableLight(uint32_t light, bool enabled) override;
		virtual void LightModelAmbient(float r, float g, float b, float a) override;
		virtual void LightColor(uint32_t light, uint32_t type, float const* color) override;
		virtual void LightColor(uint32_t light, float const* ambient, float const* diffuse, float const* specular) override;
		virtual void LightPosition(uint32_t light, float const* position) override;
		virtual void LightDirection(uint32_t light, float const* direction) override;
		virtual void MaterialColor(uint32_t type, float const* color) override;
		virtual void MaterialColor(float const* ambient, float const* diffuse, float const* specular, float const* emission, float shininess) override;

	public:
		virtual char const* GetVertexBufferName(uint32_t gdVertexFormat) override;
		virtual uint32_t    VertexBufferType(uint32_t unknown) override;
		virtual uint32_t    MaxVertices(uint32_t unknown) override;
		virtual uint32_t    GetVertices(int32_t count, bool unknown) override;
		virtual uint32_t    ContinueVertices(uint32_t unknown, uint32_t unknown2) override;
		virtual void        ReleaseVertices(uint32_t unknown) override;
		virtual void        DrawPrims(uint32_t unknown, uint32_t gdPrimType, void* prims, uint32_t count) override;
		virtual void        DrawPrimsIndexed(uint32_t unknown, uint32_t gdPrimType, uint32_t count, uint16_t* indices, void* prims, uint32_t count2) override;
		virtual void        Reset(void) override;

	private:
		void SetLastError(DriverError error);
		int  EnumerateVideoModes(void);
		void DestroyRenderWindow(void);

		/**
		 * Shared by all six blit entry points.
		 *
		 * They differ only in how they scale and how they treat alpha; the
		 * pixel upload underneath is identical, so it lives in one place.
		 */
		void UploadBlit(char const* caller,
			int32_t destLeft, int32_t destTop,
			int32_t destWidth, int32_t destHeight,
			int32_t srcWidth, int32_t srcHeight,
			uint32_t gdTexFormat, uint32_t gdType,
			void const* buffer1, void const* buffer2);

		// How many blits still owe a description of their source buffers.
		// Bounded because this is diagnostic output on a per-frame path.
		int blitProbesRemaining;

		// Same idea for geometry, but sampled one draw per distinct vertex
		// format and primitive type rather than by position in the frame, so
		// each pipeline gets described once. Sampling the first few draws
		// instead reported only tiny sub-pixel quads, which said nothing about
		// what was actually on screen.
		uint32_t probedKeys[48];
		int      probedCombinations;

		int      texMatrixProbesRemaining;
		int      mismatchReportsRemaining;
		int      coverageReportsRemaining;
		int      indexTypeWarningsRemaining;

		/**
		 * Reports each distinct configuration once, and answers false after.
		 *
		 * The ordinary trace stops after a call budget, which the interface
		 * alone exhausts long before a city finishes loading. Anything that
		 * needs observing inside a city has to be recorded by value instead of
		 * by position, so it still reports the first time it is seen no matter
		 * how late that is.
		 */
		bool NoteOnce(uint32_t bucket, uint32_t key);

		uint64_t notedKeys[192];
		int      notedCount;

		// The combiner network as the shader will read it, kept so a draw can
		// report the configuration that was actually in force for it.
		uint32_t packedCombiner[4];

		// What each stage was last told to combine with, and how. The mode
		// decides which of the two the stage actually uses: the network only
		// applies when the mode selects Combine.
		uint32_t rawCombiner[4];
		int32_t  texEnvMode[2];

		// Which stage the stage-scoped calls refer to, and whether texturing
		// is switched on for each. Both are per stage in this interface even
		// though the enable arrives through the same call as the global
		// capabilities.
		uint32_t activeTexStage;
		bool     texStageEnabled[2];

		/** Applies a texture enable to the stage TexStage last selected. */
		void SetTextureStageEnabled(bool enabled);

		/** Recomputes both stages from the mode and network and pushes them. */
		void PushCombinerState(void);

		// The last configuration reported, so an unchanged one costs a compare
		// rather than a search of everything already seen.
		uint32_t lastMultitexKey;

		/** Reports the combiner in force, once per distinct multitextured draw. */
		void NoteMultitexturedDraw(uint32_t gdVertexFormat);

		// A single frame is dumped in full, once, a little after startup so the
		// interface has settled.
		uint32_t frameCounter;
		bool     dumpFrame;
		int      dumpedDraws;
		int      frameDumpsRemaining;

		// A dump stays open over a window of frames, because the frame worth
		// seeing is the sparse one that rebuilds the scene rather than any of
		// the many that just restore it from a buffer region.
		static constexpr int kDumpWindowFrames = 240;
		static constexpr int kMaxDumpedDraws   = 4000;
		int      dumpWindowRemaining;

		// The texture on the second stage and the last texture matrix flags,
		// kept so a dumped draw can report the state it ran under.
		uint32_t stage1Texture;
		uint32_t lastTexMatrixFlags;

		/** Recomputes projection times modelview and hands it to the backend. */
		void UpdateTransform(void);

		/** Forwards blend enable and factors, which the backend keys on. */
		void PushBlendState(void);

		/** Forwards depth test, write and comparison. */
		void PushDepthState(void);

		/** Forwards the ambient tint that carries the day and night cycle. */
		void PushSceneTint(void);

		/** Forwards the alpha comparison, disabled when the capability is off. */
		void PushAlphaTest(void);

	private:
		DriverError lastError;

		std::vector<sGDMode> videoModes;
		int                  videoModeCount;
		int                  currentVideoMode;
		std::string          driverInfo;

		int windowWidth, windowHeight;
		int viewportX, viewportY, viewportWidth, viewportHeight;

		// Tracked rather than stubbed: the game reads these back through
		// IsEnabled and branches on the answer, so returning a constant would
		// distort the very call sequence this build exists to record.
		bool enabledCapabilities[kGDNumCapabilities];

		// Handed out by GenTextures and CreateTexture. The game stores these
		// and passes them back, and treats zero as "no texture", so names must
		// be unique and non-zero even while nothing is backing them yet.
		uint32_t nextTextureName;

		// Held from ClearColor until the next Clear, because the interface
		// splits what Vulkan takes as a single call.
		float clearColour[4];

		// Blend and alpha test state, held until a draw needs it. Both are
		// pipeline or shader state in Vulkan rather than commands, so they are
		// forwarded on change rather than applied immediately.
		uint32_t blendSrcFactor;
		uint32_t blendDstFactor;
		uint32_t alphaFunc;
		float    alphaRef;
		uint32_t depthCompare;
		bool     depthWrite;
		float    clearDepthValue;

		// The fixed function matrix stack, reduced to what the game uses: it
		// only ever loads whole matrices into the modelview and projection
		// slots, never pushes, pops or multiplies.
		float modelViewMatrix[16];
		float projectionMatrix[16];
		uint32_t activeMatrix;

		// The last format handed to InterleavedArrays, and the client pointer
		// it named. Draws read from that pointer, so the driver has to keep
		// both until the draw arrives.
		// The texture last bound to stage 0, as a backend handle.
		uint32_t     boundTexture;

		// The global ambient light colour and the diffuse material alpha, plus
		// whether the vertex colour feeds either. Together these are all the
		// lighting SimCity 4 uses.
		float colourMultiplier[4];
		bool  vertexColourAmbient;
		bool  vertexColourDiffuse;

		uint32_t     vertexFormat;
		uint32_t     vertexStride;
		void const*  vertexPointer;

		std::unique_ptr<VulkanBackend> vulkan;

		void* windowHandle;
	};
}
